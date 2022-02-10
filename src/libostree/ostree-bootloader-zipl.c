/*
 * Copyright (C) 2019 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-sysroot-private.h"
#include "ostree-bootloader-zipl.h"
#include "ostree-deployment-private.h"
#include "otutil.h"
#include <systemd/sd-journal.h>
#include <string.h>

#define SECURE_EXECUTION_BOOT_IMAGE     "/boot/sd-boot"
#define SECURE_EXECUTION_HOSTKEY_PATH   "/etc/se-hostkeys/"
#define SECURE_EXECUTION_HOSTKEY_PREFIX "ibm-z-hostkey"
#define SECURE_EXECUTION_INITRD_IMAGE   "/tmp/sd-initrd.img"
#define SECURE_EXECUTION_LUKS_ROOT_KEY  "/etc/luks/root"
#define SECURE_EXECUTION_LUKS_CONFIG    "/etc/crypttab"
#define SECURE_EXECUTION_RAMDISK_TOOL   PKGLIBEXECDIR "/s390x-se-luks-gencpio"

/* This is specific to zipl today, but in the future we could also
 * use it for the grub2-mkconfig case.
 */
static const char zipl_requires_execute_path[] = "boot/ostree-bootloader-update.stamp";

struct _OstreeBootloaderZipl
{
  GObject       parent_instance;

  OstreeSysroot  *sysroot;
};

typedef GObjectClass OstreeBootloaderZiplClass;

static void _ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface);
G_DEFINE_TYPE_WITH_CODE (OstreeBootloaderZipl, _ostree_bootloader_zipl, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (OSTREE_TYPE_BOOTLOADER, _ostree_bootloader_zipl_bootloader_iface_init));

static gboolean
_ostree_bootloader_zipl_query (OstreeBootloader *bootloader,
                                   gboolean         *out_is_active,
                                   GCancellable     *cancellable,
                                   GError          **error)
{
  /* We don't auto-detect this one; should be explicitly chosen right now.
   * see also https://github.com/coreos/coreos-assembler/pull/849
   */
  *out_is_active = FALSE;
  return TRUE;
}

static const char *
_ostree_bootloader_zipl_get_name (OstreeBootloader *bootloader)
{
  return "zipl";
}

static gboolean
_ostree_bootloader_zipl_write_config (OstreeBootloader  *bootloader,
                                          int                bootversion,
                                          GPtrArray         *new_deployments,
                                          GCancellable      *cancellable,
                                          GError           **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Write our stamp file */
  if (!glnx_file_replace_contents_at (self->sysroot->sysroot_fd, zipl_requires_execute_path,
                                      (guint8*)"", 0, GLNX_FILE_REPLACE_NODATASYNC,
                                      cancellable, error))
    return FALSE;

  return TRUE;
}

static gboolean
_ostree_secure_execution_get_keys (GPtrArray **keys,
                                   GCancellable *cancellable,
                                   GError **error)
{
  g_auto (GLnxDirFdIterator) it = { 0,};
  if ( !glnx_dirfd_iterator_init_at (-1, SECURE_EXECUTION_HOSTKEY_PATH, TRUE, &it, error))
    return glnx_prefix_error (error, "s390x SE: looking for SE keys");

  g_autoptr(GPtrArray) ret_keys = g_ptr_array_new_with_free_func (g_free);
  while (TRUE)
    {
      struct dirent *dent = NULL;
      if (!glnx_dirfd_iterator_next_dent (&it, &dent, cancellable, error))
        return FALSE;

      if (!dent)
        break;

      if (g_str_has_prefix (dent->d_name, SECURE_EXECUTION_HOSTKEY_PREFIX))
        g_ptr_array_add (ret_keys, g_build_filename (SECURE_EXECUTION_HOSTKEY_PATH, dent->d_name, NULL));
    }

  *keys = g_steal_pointer (&ret_keys);
  return TRUE;
}

static gboolean
_ostree_secure_execution_get_bls_config (OstreeBootloaderZipl *self,
                                         int bootversion,
                                         gchar **vmlinuz,
                                         gchar **initramfs,
                                         gchar **options,
                                         GCancellable *cancellable,
                                         GError **error)
{
  g_autoptr (GPtrArray) configs = NULL;
  if ( !_ostree_sysroot_read_boot_loader_configs (self->sysroot, bootversion, &configs, cancellable, error))
    return glnx_prefix_error (error, "s390x SE: loading bls configs");

  if (!configs || configs->len == 0)
    return glnx_throw (error, "s390x SE: no bls config");

  OstreeBootconfigParser *parser = (OstreeBootconfigParser *) g_ptr_array_index (configs, 0);
  const gchar *val = NULL;

  val = ostree_bootconfig_parser_get (parser, "linux");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"linux\" key in bootloader config");
  *vmlinuz = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "initrd");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"initrd\" key in bootloader config");
  *initramfs = g_build_filename ("/boot", val, NULL);

  val = ostree_bootconfig_parser_get (parser, "options");
  if (!val)
    return glnx_throw (error, "s390x SE: no \"options\" key in bootloader config");
  *options = g_strdup(val);

  return TRUE;
}

static gboolean
_ostree_secure_execution_luks_key_exists (void)
{
  return (access(SECURE_EXECUTION_LUKS_ROOT_KEY, F_OK) == 0 &&
          access(SECURE_EXECUTION_LUKS_CONFIG, F_OK) == 0);
}

static gboolean
_ostree_secure_execution_enable_luks(const gchar* initramfs, GError **error)
{
  const char *const argv[] = {SECURE_EXECUTION_RAMDISK_TOOL, initramfs, SECURE_EXECUTION_INITRD_IMAGE, NULL};
  g_autofree gchar *out = NULL;
  g_autofree gchar *err = NULL;
  int status = 0;
  if (!g_spawn_sync (NULL, (char**)argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, &out, &err, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning %s", SECURE_EXECUTION_RAMDISK_TOOL);

  if (!g_spawn_check_exit_status (status, error))
    {
      g_printerr("s390x SE: `%s` stdout: %s\n", SECURE_EXECUTION_RAMDISK_TOOL, out);
      g_printerr("s390x SE: `%s` stderr: %s\n", SECURE_EXECUTION_RAMDISK_TOOL, err);
      return glnx_prefix_error(error, "s390x SE: `%s` failed", SECURE_EXECUTION_RAMDISK_TOOL);
    }

  sd_journal_print(LOG_INFO, "s390x SE: luks key added to initrd");
  return TRUE;
}

static gboolean
_ostree_secure_execution_generate_sdboot (const gchar *vmlinuz,
                                          const gchar *initramfs,
                                          const gchar *options,
                                          const GPtrArray *keys,
                                          GError **error)
{
  g_assert (vmlinuz && initramfs && options && keys && keys->len);
  sd_journal_print(LOG_INFO, "s390x SE: kernel: %s", vmlinuz);
  sd_journal_print(LOG_INFO, "s390x SE: initrd: %s", initramfs);
  sd_journal_print(LOG_INFO, "s390x SE: kargs: %s", options);

  g_autofree char *cmdline = g_strdup ("/tmp/sd_boot.parmfile.XXXXXX");
  glnx_autofd int fd       = g_mkstemp (cmdline);
  if (glnx_loop_write (fd, options, strlen (options)) < 0)
    return glnx_throw_errno_prefix (error, "s390x SE: creating %s", cmdline);
  glnx_close_fd(&fd);

  const gchar *ramdisk = initramfs;
  if (_ostree_secure_execution_luks_key_exists ())
    {
      if ( !_ostree_secure_execution_enable_luks (initramfs, error))
        return FALSE;
      ramdisk = SECURE_EXECUTION_INITRD_IMAGE;
    }

  g_autoptr(GPtrArray) argv = g_ptr_array_new ();
  g_ptr_array_add (argv, "genprotimg");
  g_ptr_array_add (argv, "-i");
  g_ptr_array_add (argv, vmlinuz);
  g_ptr_array_add (argv, "-r");
  g_ptr_array_add (argv, ramdisk);
  g_ptr_array_add (argv, "-p");
  g_ptr_array_add (argv, cmdline);
  for (guint i = 0; i < keys->len; ++i)
    {
      gchar *key = g_ptr_array_index (keys, i);
      g_ptr_array_add (argv, "-k");
      g_ptr_array_add (argv, key);
      sd_journal_print(LOG_INFO, "s390x SE: key[%d]: %s", i + 1, key);
    }
  g_ptr_array_add (argv, "--no-verify");
  g_ptr_array_add (argv, "-o");
  g_ptr_array_add (argv, SECURE_EXECUTION_BOOT_IMAGE);
  g_ptr_array_add (argv, NULL);

  gint status = 0;
  if (!g_spawn_sync (NULL, (char**)argv->pdata, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, NULL, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning genprotimg");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error(error, "s390x SE: `genprotimg` failed");

  sd_journal_print(LOG_INFO, "s390x SE: `%s` generated", SECURE_EXECUTION_BOOT_IMAGE);
  unlink (cmdline);
  return TRUE;
}

static gboolean
_ostree_secure_execution_call_zipl (GError **error)
{
  int status = 0;
  const char *const zipl_argv[] = {"zipl", "-V", "-t", "/boot", "-i", SECURE_EXECUTION_BOOT_IMAGE, NULL};
  if (!g_spawn_sync (NULL, (char**)zipl_argv, NULL, G_SPAWN_SEARCH_PATH,
                       NULL, NULL, NULL, NULL, &status, error))
    return glnx_prefix_error(error, "s390x SE: spawning zipl");

  if (!g_spawn_check_exit_status (status, error))
    return glnx_prefix_error(error, "s390x SE: `zipl` failed");

  sd_journal_print(LOG_INFO, "s390x SE: `sd-boot` zipled");
  return TRUE;
}

static gboolean
_ostree_secure_execution_enable (OstreeBootloaderZipl *self,
                                 int bootversion,
                                 GPtrArray *keys,
                                 GCancellable *cancellable,
                                 GError **error)
{
  g_autofree gchar* vmlinuz = NULL;
  g_autofree gchar* initramfs = NULL;
  g_autofree gchar* options = NULL;

  gboolean rc =
      _ostree_secure_execution_get_bls_config (self, bootversion, &vmlinuz, &initramfs, &options, cancellable, error) &&
      _ostree_secure_execution_generate_sdboot (vmlinuz, initramfs, options, keys, error) &&
      _ostree_secure_execution_call_zipl (error);

  return rc;
}


static gboolean
_ostree_bootloader_zipl_post_bls_sync (OstreeBootloader  *bootloader,
                                       int bootversion,
                                       GCancellable  *cancellable,
                                       GError       **error)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (bootloader);

  /* Note that unlike the grub2-mkconfig backend, we make no attempt to
   * chroot().
   */
  g_assert (self->sysroot->booted_deployment);

  if (!glnx_fstatat_allow_noent (self->sysroot->sysroot_fd, zipl_requires_execute_path, NULL, 0, error))
    return FALSE;

  /* If there's no stamp file, nothing to do */
  if (errno == ENOENT)
    return TRUE;

  /* Try with Secure Execution */
  g_autoptr(GPtrArray) keys = NULL;
  if (!_ostree_secure_execution_get_keys (&keys, cancellable, error))
    return FALSE;
  if (keys && keys->len)
    return _ostree_secure_execution_enable (self, bootversion, keys, cancellable, error);

  /* Fallback to non-SE setup */
  const char *const zipl_argv[] = {"zipl", NULL};
  int estatus;
  if (!g_spawn_sync (NULL, (char**)zipl_argv, NULL, G_SPAWN_SEARCH_PATH,
                     NULL, NULL, NULL, NULL, &estatus, error))
    return FALSE;
  if (!g_spawn_check_exit_status (estatus, error))
    return FALSE;
  if (!glnx_unlinkat (self->sysroot->sysroot_fd, zipl_requires_execute_path, 0, error))
    return FALSE;
  return TRUE;
}

static void
_ostree_bootloader_zipl_finalize (GObject *object)
{
  OstreeBootloaderZipl *self = OSTREE_BOOTLOADER_ZIPL (object);

  g_clear_object (&self->sysroot);

  G_OBJECT_CLASS (_ostree_bootloader_zipl_parent_class)->finalize (object);
}

void
_ostree_bootloader_zipl_init (OstreeBootloaderZipl *self)
{
}

static void
_ostree_bootloader_zipl_bootloader_iface_init (OstreeBootloaderInterface *iface)
{
  iface->query = _ostree_bootloader_zipl_query;
  iface->get_name = _ostree_bootloader_zipl_get_name;
  iface->write_config = _ostree_bootloader_zipl_write_config;
  iface->post_bls_sync = _ostree_bootloader_zipl_post_bls_sync;
}

void
_ostree_bootloader_zipl_class_init (OstreeBootloaderZiplClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = _ostree_bootloader_zipl_finalize;
}

OstreeBootloaderZipl *
_ostree_bootloader_zipl_new (OstreeSysroot *sysroot)
{
  OstreeBootloaderZipl *self = g_object_new (OSTREE_TYPE_BOOTLOADER_ZIPL, NULL);
  self->sysroot = g_object_ref (sysroot);
  return self;
}
