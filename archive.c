// 2011 Ninjas
// Licensed under the terms of the GNU GPL, version 2
// http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt

#include "tools.h"
#include "types.h"
#include "common.h"
#include "keys.h"

#include "paged_file.h"
#include "archive.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

static u8 device_id[0x10] = {0};
static int device_id_set = FALSE;
const char *keys_conf_path = "keys.conf";
Key *keys = NULL;
int num_keys = 0;


ChainedList *
chained_list_append (ChainedList *list, void *data)
{
  ChainedList *l = malloc (sizeof(ChainedList));
  ChainedList *c;

  l->data = data;
  l->next = NULL;
  if (list == NULL)
    return l;

  c = list;
  while (c->next) c = c->next;
  c->next = l;

  return list;
}

void
chained_list_foreach (ChainedList *list, ChainedListForeachCallback cb, void *user_data)
{
  ChainedList *current = list;

  while (current != NULL) {
    cb (current->data, user_data);
    current = current->next;
  }
}

void
chained_list_free (ChainedList *list)
{
  ChainedList *current = list;
  ChainedList *next;

  while (current != NULL) {
    next = current->next;
    free (current);
    current = next;
  }
}

static void
generate_random_key_seed (u8 *seed)
{
  get_rand (seed, 0x14);
}

static void sc_encrypt (u32 type, u8 *laid_paid, u8 *iv, u8 *in, u32 in_size, u8 *out);

static void
vtrm_encrypt_with_portability (u32 type, u8 *buffer, u8 *iv)
{
  u32 real_type;
  u8 laid_paid[16] = {0};

  switch ( type )
  {
    case 0:
      real_type = 1;
      break;
    case 3:
      real_type = 5;
      break;
    case 1:
      real_type = 3;
      break;
    case 2:
      real_type = 2;
      break;
    default:
      die ("vtrm_encrypt_with_portability Unknown method\n");
      break;
  }
  sc_encrypt (real_type, laid_paid, iv, buffer, 0x40, buffer);
}

static void
vtrm_encrypt (u32 type, u8 *buffer, u8 *iv)
{
  u64 laid_paid[2];

  laid_paid[0] = TO_BE (64, 0x1070000002000001ULL);
  switch ( type )
  {
    case 0:
      laid_paid[0] = laid_paid[1] = -1;
      break;
    case 1:
      laid_paid[1] = TO_BE (64, 0x1070000000000001ULL);
      break;
    case 2:
      laid_paid[1] = 0;
    case 3:
      laid_paid[1] = TO_BE (64, 0x10700003FF000001ULL);
      break;
    default:
      die ("vtrm_encrypt Unknown method\n");
      break;
  }
  sc_encrypt (3, (u8 *)laid_paid, iv, buffer, 0x40, buffer);
}

static void
sc_encrypt (u32 type, u8 *laid_paid, u8 *iv, u8 *in, u32 in_size, u8 *out)
{
  Key *sc_key;
  u8 key[16];
  int i;

  if (type > 5)
    die ("sc_encrypt: Invalid key type\n");

  if (keys == NULL) {
    keys = keys_load_from_file (keys_conf_path, &num_keys);
    if (keys == NULL)
      die ("Unable to load necessary keys from : %s\n", keys_conf_path);
  }

  sc_key = keys_find_by_revision (keys, num_keys, KEY_TYPE_SC, type);
  if (sc_key == NULL)
      die ("sc_encrypt: Unknown key\n");

  memcpy (key, sc_key->key, 16);
  for (i = 0; i < 16; i++)
    key[i] ^= laid_paid[i];
  aes128cbc_enc (key, iv, in, in_size, out);
}

static int
archive_gen_keys (DatFileHeader *header, u8 *key, u8 *iv, u8 *hmac)
{
  u8 buffer[0x40];
  u8 zero_iv[0x10];

  memset (buffer, 0, 0x40);
  memset (zero_iv, 0, 0x10);
  if (header->size == 0x30) {
    if (!device_id_set)
      die ("Device ID is not set. You must set it with the command SetDeviceID\n");
    memcpy (buffer, device_id, 0x10);
    vtrm_encrypt (3, buffer, zero_iv);
  } else {
    memcpy (buffer, header->key_seed, 0x14);
    vtrm_encrypt_with_portability (1, buffer, zero_iv);
  }
  memcpy (key, buffer, 0x10);
  memcpy (iv, buffer + 0x10, 0x10);
  memset (hmac, 0, 0x40);
  memcpy (hmac, buffer + 0x2C, 0x14);

  return TRUE;
}

int
archive_open (const char *path, PagedFile *file, DatFileHeader *dat_header)
{
  u8 key[0x10];
  u8 iv[0x10];
  u8 hmac[0x40];


  if (!paged_file_open (file, path, TRUE)) {
    DBG ("Couldn't open file %s\n", path);
    goto end;
  }

  if (paged_file_read (file, dat_header, sizeof(DatFileHeader)) != sizeof(DatFileHeader)) {
    DBG ("Couldn't read dat file header\n");
    goto end;
  }

  dat_header->size = FROM_LE (32, dat_header->size);
  dat_header->type = FROM_BE (32, dat_header->type);

  if (dat_header->size != 0x40 && dat_header->size != 0x30) {
    DBG ("Invalid dat header size : %X\n", dat_header->size);
    goto end;
  }

  if (dat_header->type != 5) {
    DBG ("Header type must be 5, not : %X\n", dat_header->type);
    goto end;
  }

  if (!archive_gen_keys (dat_header, key, iv, hmac)) {
    DBG ("Error generating keys\n");
    goto end;
  }

  paged_file_hash (file, hmac);
  paged_file_crypt (file, key, iv);

  return TRUE;
 end:
  paged_file_close (file);
  return FALSE;
}

int
archive_decrypt (const char *path, const char *to)
{
  FILE *fd = NULL;
  DatFileHeader dat_header;
  PagedFile file = {0};
  int read;
  int ret = FALSE;

  if (!archive_open (path, &file, &dat_header)) {
    DBG ("Couldn't open file %s\n", path);
    goto end;
  }

  fd = fopen (to, "wb");
  if (!fd) {
    DBG ("Couldn't open output file %s\n", to);
    goto end;
  }

  dat_header.size = TO_LE (32, dat_header.size);
  dat_header.type = TO_BE (32, dat_header.type);
  fwrite (&dat_header, sizeof(dat_header), 1, fd);

  do {
    u8 buffer[1024];
    read = paged_file_read (&file, buffer, sizeof(buffer));
    fwrite (buffer, read, 1, fd);
  } while (read > 0);


  ret = TRUE;
 end:
  if (fd)
    fclose (fd);

  paged_file_close (&file);

  if (ret && memcmp (dat_header.hash, file.digest, 0x14) != 0) {
    DBG ("HMAC hash does not match\n");
    ret = FALSE;
  }
  return ret;
}

int
index_archive_read (ArchiveIndex *archive_index, const char *path)
{
  PagedFile file = {0};
  DatFileHeader dat_header;

  archive_index->header.id = archive_index->footer.archive2_size = 0;
  archive_index->files = archive_index->dirs = NULL;
  archive_index->total_files = archive_index->total_file_sizes = archive_index->total_dirs = 0;

  if (!archive_open (path, &file, &dat_header)) {
    DBG ("Couldn't open file %s\n", path);
    goto end;
  }

  if (paged_file_read (&file, &archive_index->header, sizeof(archive_index->header)) != sizeof(archive_index->header)) {
    DBG ("Couldn't read archive encrypted header\n");
    goto end;
  }
  archive_index->header.index = FROM_BE (32, archive_index->header.index);

  while(1) {
    ArchiveFile *archive_file = malloc (sizeof(ArchiveFile));
    if (paged_file_read (&file, archive_file, sizeof(ArchiveFile)) != sizeof(ArchiveFile)) {
      DBG ("Couldn't read file entry\n");
      goto end;
    }
    if (archive_file->eos.zero == 0) {
      archive_index->total_files = FROM_BE (64, archive_file->eos.total_files);
      archive_index->total_file_sizes = FROM_BE (64, archive_file->eos.total_file_sizes);
      free (archive_file);
      break;
    }
    archive_file->stat.mode = FROM_BE (32, archive_file->stat.mode);
    archive_file->stat.uid = FROM_BE (32, archive_file->stat.uid);
    archive_file->stat.gid = FROM_BE (32, archive_file->stat.gid);
    archive_file->stat.atime = FROM_BE (64, archive_file->stat.atime);
    archive_file->stat.mtime = FROM_BE (64, archive_file->stat.mtime);
    archive_file->stat.ctime = FROM_BE (64, archive_file->stat.ctime);
    archive_file->stat.file_size = FROM_BE (64, archive_file->stat.file_size);
    archive_file->stat.block_size = FROM_BE (64, archive_file->stat.block_size);
    archive_file->flags = FROM_BE (32, archive_file->flags);
    archive_index->files = chained_list_append (archive_index->files, archive_file);
    DBG ("File : %s\n", archive_file->path);
  }
  while(1) {
    ArchiveDirectory *archive_dir = malloc (sizeof(ArchiveDirectory));
    if (paged_file_read (&file, archive_dir, sizeof(ArchiveDirectory)) != sizeof(ArchiveDirectory)) {
      DBG ("Couldn't read directory entry\n");
      goto end;
    }
    if (archive_dir->eos.zero == 0) {
      archive_index->total_dirs = FROM_BE (64, archive_dir->eos.total_dirs);
      free (archive_dir);
      break;
    }
    archive_dir->stat.mode = FROM_BE (32, archive_dir->stat.mode);
    archive_dir->stat.uid = FROM_BE (32, archive_dir->stat.uid);
    archive_dir->stat.gid = FROM_BE (32, archive_dir->stat.gid);
    archive_dir->stat.atime = FROM_BE (64, archive_dir->stat.atime);
    archive_dir->stat.mtime = FROM_BE (64, archive_dir->stat.mtime);
    archive_dir->stat.ctime = FROM_BE (64, archive_dir->stat.ctime);
    archive_dir->stat.file_size = FROM_BE (64, archive_dir->stat.file_size);
    archive_dir->stat.block_size = FROM_BE (64, archive_dir->stat.block_size);
    archive_dir->flags = FROM_BE (32, archive_dir->flags);
    archive_index->dirs = chained_list_append (archive_index->dirs, archive_dir);
    DBG ("Directory : %s\n", archive_dir->path);
  }

  if (archive_index->header.archive_type == 5) {
    if (paged_file_read (&file, &archive_index->footer, sizeof(archive_index->footer)) != sizeof(archive_index->footer)) {
      DBG ("Couldn't read index archive footer\n");
      goto end;
    }
    archive_index->footer.archive2_size = FROM_BE (64, archive_index->footer.archive2_size);
  }

  paged_file_close (&file);

  if (memcmp (dat_header.hash, file.digest, 0x14) != 0) {
    DBG ("HMAC hash does not match\n");
    return FALSE;
  }

  return TRUE;

 end:
  paged_file_close (&file);
  return FALSE;
}


int
index_archive_write (ArchiveIndex *archive_index, const char *path)
{
  FILE *fd = NULL;
  PagedFile file = {0};
  DatFileHeader dat_header;
  ArchiveFile file_eos = {0};
  ArchiveDirectory dir_eos = {0};
  ChainedList *list;
  u8 key[0x10];
  u8 iv[0x10];
  u8 hmac[0x40];

  if (!paged_file_open (&file, path, FALSE)) {
    DBG ("Couldn't open file %s\n", path);
    goto end;
  }

  dat_header.size = TO_LE (32, 0x40);
  dat_header.type = TO_BE (32, 0x05);
  generate_random_key_seed (dat_header.key_seed);
  memset (dat_header.padding, 0, 0x10);

  if (paged_file_write (&file, &dat_header, sizeof(dat_header)) != sizeof(dat_header)) {
    DBG ("Couldn't write file dat header\n");
    goto end;
  }

  if (!archive_gen_keys (&dat_header, key, iv, hmac)) {
    DBG ("Error generating keys\n");
    goto end;
  }

  paged_file_flush (&file);
  paged_file_hash (&file, hmac);
  paged_file_crypt (&file, key, iv);

  archive_index->header.index = 0;
  archive_index->header.archive_type = 5;
  archive_index->header.id_type = 1;
  archive_index->header.padding = 0;
  if (paged_file_write (&file, &archive_index->header, sizeof(archive_index->header)) != sizeof(archive_index->header)) {
    DBG ("Couldn't write archive index header\n");
    goto end;
  }

  archive_index->total_files = 0;
  archive_index->total_file_sizes = 0;
  for (list = archive_index->files; list; list = list->next) {
    ArchiveFile *archive_file = list->data;

    archive_index->total_files++;
    archive_index->total_file_sizes += archive_file->stat.file_size;
    archive_file->stat.mode = TO_BE (32, archive_file->stat.mode);
    archive_file->stat.uid = TO_BE (32, archive_file->stat.uid);
    archive_file->stat.gid = TO_BE (32, archive_file->stat.gid);
    archive_file->stat.atime = TO_BE (64, archive_file->stat.atime);
    archive_file->stat.mtime = TO_BE (64, archive_file->stat.mtime);
    archive_file->stat.ctime = TO_BE (64, archive_file->stat.ctime);
    archive_file->stat.file_size = TO_BE (64, archive_file->stat.file_size);
    archive_file->stat.block_size = TO_BE (64, archive_file->stat.block_size);
    archive_file->flags = TO_BE (32, archive_file->flags);
    if (paged_file_write (&file, archive_file, sizeof(ArchiveFile)) != sizeof(ArchiveFile)) {
      DBG ("Couldn't write file entry\n");
      goto end;
    }
  }
  file_eos.eos.zero = 0;
  file_eos.eos.total_files = TO_BE (64, archive_index->total_files);
  file_eos.eos.total_file_sizes = TO_BE (64, archive_index->total_file_sizes);
  if (paged_file_write (&file, &file_eos, sizeof(ArchiveFile)) != sizeof(ArchiveFile)) {
    DBG ("Couldn't write file EOS\n");
    goto end;
  }
  archive_index->total_dirs = 0;
  for (list = archive_index->dirs; list; list = list->next) {
    ArchiveDirectory *archive_dir = list->data;

    archive_index->total_dirs++;
    archive_dir->stat.mode = TO_BE (32, archive_dir->stat.mode);
    archive_dir->stat.uid = TO_BE (32, archive_dir->stat.uid);
    archive_dir->stat.gid = TO_BE (32, archive_dir->stat.gid);
    archive_dir->stat.atime = TO_BE (64, archive_dir->stat.atime);
    archive_dir->stat.mtime = TO_BE (64, archive_dir->stat.mtime);
    archive_dir->stat.ctime = TO_BE (64, archive_dir->stat.ctime);
    archive_dir->stat.file_size = TO_BE (64, archive_dir->stat.file_size);
    archive_dir->stat.block_size = TO_BE (64, archive_dir->stat.block_size);
    archive_dir->flags = TO_BE (32, archive_dir->flags);
    if (paged_file_write (&file, archive_dir, sizeof(ArchiveDirectory)) != sizeof(ArchiveDirectory)) {
      DBG ("Couldn't write dir entry\n");
      goto end;
    }
  }
  dir_eos.eos.zero = 0;
  dir_eos.eos.total_dirs = TO_BE (64, archive_index->total_dirs);
  if (paged_file_write (&file, &dir_eos, sizeof(ArchiveDirectory)) != sizeof(ArchiveDirectory)) {
    DBG ("Couldn't write dir EOS\n");
    goto end;
  }

  if (archive_index->header.archive_type == 5) {
    archive_index->footer.archive2_size = TO_BE (64, archive_index->footer.archive2_size);
    archive_index->footer.padding = 0;
    if (paged_file_write (&file, &archive_index->footer, sizeof(archive_index->footer)) != sizeof(archive_index->footer)) {
      DBG ("Couldn't write archive index footer\n");
      goto end;
    }
  }

  paged_file_flush (&file);
  fd = file.fd;
  file.fd = NULL;
  paged_file_close (&file);

  fseek (fd, 8, SEEK_SET);
  fwrite (file.digest, 0x14, 1, fd);
  fclose (fd);


  return TRUE;

 end:
  paged_file_close (&file);
  return FALSE;
}

static void
index_archive_free_cb (void *to_free, void *ignore)
{
  free (to_free);
}

int
index_archive_free (ArchiveIndex *archive_index)
{
  chained_list_foreach (archive_index->files, index_archive_free_cb, NULL);
  chained_list_foreach (archive_index->dirs, index_archive_free_cb, NULL);
  chained_list_free (archive_index->files);
  chained_list_free (archive_index->dirs);
}

int
data_archive_read (ArchiveData *archive_data, const char *path)
{
  PagedFile file = {0};
  DatFileHeader dat_header;
  int read;

  archive_data->header.id = archive_data->header.index = archive_data->header.archive_type = archive_data->header.id_type = 0;

  if (!archive_open (path, &file, &dat_header)) {
    DBG ("Couldn't open file %s\n", path);
    goto end;
  }

  if (paged_file_read (&file, &archive_data, sizeof(archive_data)) != sizeof(archive_data)) {
    DBG ("Couldn't read encrypted header\n");
    goto end;
  }
  archive_data->header.index = FROM_BE (32, archive_data->header.index);

  do {
    u8 buffer[1024];
    read = paged_file_read (&file, buffer, sizeof(buffer));
  } while (read > 0);

  paged_file_close (&file);

  if (memcmp (dat_header.hash, file.digest, 0x14) != 0) {
    DBG ("HMAC hash does not match\n");
    return FALSE;
  }

  return TRUE;

 end:
  paged_file_close (&file);
  return FALSE;
}

int
archive_find_file (ArchiveIndex *archive_index, const char *path,
    const char *filename, ArchiveFile **archive_file, u32 *index, u64 *position)
{
  ChainedList *current = archive_index->files;
  struct stat stat_buf;
  char data_path[1024];
  u64 file_size = 0;

  *index = 0;
  *position = 0x50; // skip header
  snprintf (data_path, sizeof(data_path), "%s/%s_%02d.dat", path, archive_index->prefix, *index);
  if (stat (data_path, &stat_buf) != 0)
    return FALSE;

  while (current != NULL) {
    ArchiveFile *file = current->data;
    if (strcmp (file->path, filename) == 0) {
      *archive_file = file;
      return TRUE;
    }
    current = current->next;
    if (*position + file->stat.file_size >= (u64) stat_buf.st_size) {
      *position = 0x50;
      (*index)++;
      snprintf (data_path, sizeof(data_path), "%s/%s_%02d.dat", path, archive_index->prefix, *index);
      if (stat (data_path, &stat_buf) != 0)
        return FALSE;
    } else {
      *position += file->stat.file_size;
    }
  }

  return FALSE;
}

int
archive_extract (ArchiveIndex *archive_index, const char *path, u32 index,
    u64 offset, u64 size, const char *output)
{
  char filename[1024];
  DatFileHeader dat_header;
  ArchiveHeader archive_header;
  PagedFile in = {0};
  PagedFile out = {0};

  if (!paged_file_open (&out, output, FALSE))
    die ("Couldn't open output file : %s\n", output);

  while (size > 0) {
    int read;

    snprintf (filename, sizeof(filename), "%s/%s_%02d.dat", path, archive_index->prefix, index);
    if (!archive_open (filename, &in, &dat_header))
      die ("Couldn't open archive %d\n", index);

    if (paged_file_read (&in, &archive_header, sizeof(archive_header)) != sizeof(archive_header))
      die ("Couldn't read archive header\n");
    if (archive_header.id != archive_index->header.id)
      die ("Wrong archive ID\n");
    if (FROM_BE (32, archive_header.index) != index)
      die ("Wrong archive index\n");
    paged_file_seek (&in, offset);
    index++;
    offset = 0x50;

    read = paged_file_splice (&out, &in, size);
    size -= read;
    paged_file_close (&in);
  }

  paged_file_close (&out);

  return TRUE;
}

int
archive_extract_file (const char *path, const char *filename, const char *output)
{
  ArchiveIndex archive;
  ArchiveFile *file = NULL;
  char buffer[1024];
  u32 index;
  u64 offset;
  u64 size;
  int ret = FALSE;

  /* Try to extract from archive.dat */
  archive.prefix = "archive";
  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive.prefix);
  if (!index_archive_read (&archive, buffer))
    die ("Unable to read index archive\n");

  if (archive_find_file (&archive, path, filename, &file, &index, &offset)) {
    ret = archive_extract (&archive, path, index, offset, file->stat.file_size, output);
    index_archive_free (&archive);
  } else if (device_id_set) {
    ArchiveIndex archive2;

    archive2.prefix = "archive2";
    /* Extract from archive2.dat */
    snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive2.prefix);
    if (!index_archive_read (&archive2, buffer))
      die ("Unable to read index archive\n");

    if (archive_find_file (&archive2, path, filename, &file, &index, &offset))
      ret = archive_extract (&archive2, path, index, offset, file->stat.file_size, output);
      index_archive_free (&archive2);
  }

  return ret;
}

static void
archive_append_file_to_list_cb (ArchiveFile *file, ChainedList **list)
{
  *list = chained_list_append (*list, file);
}

int
archive_extract_path (const char *path, const char *match, const char *output)
{
  ChainedList *all_files = NULL;
  ChainedList *current;
  ArchiveIndex archive = {0};
  ArchiveIndex archive2 = {0};
  ArchiveFile *file = NULL;
  char buffer[2048];
  u32 index;
  u64 offset;
  u64 size;
  int match_len = strlen (match);

  /* Try to extract from archive.dat */
  archive.prefix = "archive";
  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive.prefix);
  if (!index_archive_read (&archive, buffer))
    die ("Unable to read index archive\n");

  chained_list_foreach (archive.files,
      (ChainedListForeachCallback) archive_append_file_to_list_cb, &all_files);

  /* Try to extract from archive2.dat */
  if (device_id_set) {
    archive2.prefix = "archive2";
    snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive2.prefix);
    if (!index_archive_read (&archive2, buffer))
      die ("Unable to read index archive\n");

    chained_list_foreach (archive2.files,
        (ChainedListForeachCallback) archive_append_file_to_list_cb, &all_files);
  }

  current = all_files;
  while (current != NULL) {
    ArchiveFile *file = current->data;
    if (strncmp (file->path, match, match_len) == 0) {
      int i;

      snprintf (buffer, sizeof(buffer), "%s/%s", output, file->path);
      i = strlen (buffer);
      while (i > 0 && buffer[i] != '/') i--;
      if (i > 0) {
        buffer[i] = 0;
        mkdir_recursive (buffer);
        buffer[i] = '/';
      }
      if (!archive_extract_file (path, file->path, buffer))
        return FALSE;
    }
    current = current->next;
  }
  index_archive_free (&archive);
  index_archive_free (&archive2);
  chained_list_free (all_files);

  return TRUE;
}

int
archive_rename_file (const char *path, const char *filename, const char *destination)
{
  ArchiveIndex archive;
  ArchiveFile *file = NULL;
  char buffer[1024];
  u32 index;
  u64 offset;
  u64 size;

  /* Try to extract from archive.dat */
  archive.prefix = "archive";
  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive.prefix);
  if (!index_archive_read (&archive, buffer))
    die ("Unable to read index archive\n");

  if (archive_find_file (&archive, path, filename, &file, &index, &offset)) {
    strcpy (file->path, destination);
    if (!index_archive_write (&archive, buffer))
      die ("Unable to write index archive\n");
  }
  index_archive_free (&archive);

  if (device_id_set) {
    ArchiveIndex archive2;

    archive2.prefix = "archive2";
    /* Extract from archive2.dat */
    snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive2.prefix);
    if (!index_archive_read (&archive2, buffer))
      die ("Unable to read index archive\n");

    if (archive_find_file (&archive2, path, filename, &file, &index, &offset)) {
      strcpy (file->path, destination);
      if (!index_archive_write (&archive2, buffer))
        die ("Unable to write index archive\n");
    }
    index_archive_free (&archive2);
  }

  return FALSE;
}

int
archive_rename_path (const char *path, const char *match, const char *destination)
{
  ChainedList *all_files = NULL;
  ChainedList *current;
  ArchiveIndex archive = {0};
  ArchiveIndex archive2 = {0};
  ArchiveFile *file = NULL;
  char buffer[2048];
  u32 index;
  u64 offset;
  u64 size;
  int match_len = strlen (match);

  /* Try to extract from archive.dat */
  archive.prefix = "archive";
  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive.prefix);
  if (!index_archive_read (&archive, buffer))
    die ("Unable to read index archive\n");

  chained_list_foreach (archive.files,
      (ChainedListForeachCallback) archive_append_file_to_list_cb, &all_files);

  /* Try to extract from archive2.dat */
  if (device_id_set) {
    archive2.prefix = "archive2";
    snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive2.prefix);
    if (!index_archive_read (&archive2, buffer))
      die ("Unable to read index archive\n");

    chained_list_foreach (archive2.files,
        (ChainedListForeachCallback) archive_append_file_to_list_cb, &all_files);
  }

  current = all_files;
  while (current != NULL) {
    ArchiveFile *file = current->data;
    if (strncmp (file->path, match, match_len) == 0) {
      if (!archive_rename_file (path, file->path, destination))
        return FALSE;
    }
    current = current->next;
  }

  index_archive_free (&archive);
  index_archive_free (&archive2);
  chained_list_free (all_files);

  return TRUE;
}

int
archive_dump (const char *path, const char *prefix, const char *output)
{
  ChainedList *list = NULL;
  char buffer[0x10000];
  ArchiveIndex archive_index;
  DatFileHeader dat_header;
  ArchiveHeader archive_header;
  PagedFile pf = {0};
  u32 index = 0;
  int open = FALSE;

  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, prefix);
  archive_index.prefix = prefix;
  if (!index_archive_read (&archive_index, buffer))
    die ("Unable to read index archive\n");

  for (list = archive_index.dirs; list; list = list->next) {
    ArchiveDirectory *dir = list->data;

    snprintf (buffer, sizeof(buffer), "%s/%s", output, dir->path);
    if (mkdir_recursive (buffer) != 0)
      die ("Error making directories\n");
  }

  for (list = archive_index.files; list; list = list->next) {
    ArchiveFile *file = list->data;
    FILE *fd;
    u64 len = file->stat.file_size;

    snprintf (buffer, sizeof(buffer), "%s/%s", output, file->path);
    fd = fopen (buffer, "wb");
    if (!fd)
      die ("Error opening output file %s\n", buffer);
    while (len > 0) {
      int read;
      u32 size = len;

      if (!open) {
        snprintf (buffer, sizeof(buffer), "%s/%s_%02d.dat", path, prefix, index);
        if (!archive_open (buffer, &pf, &dat_header)) {
          die ("Couldn't open archive %d\n", index);
        }

        if (paged_file_read (&pf, &archive_header, sizeof(archive_header)) != sizeof(archive_header))
          die ("Couldn't read archive header\n");
        if (archive_header.id != archive_index.header.id)
          die ("Wrong archive ID\n");
        if (FROM_BE (32, archive_header.index) != index)
          die ("Wrong archive index\n");
        index++;
        open = TRUE;
      }

      if (size > sizeof(buffer))
        size = sizeof(buffer);
      read = paged_file_read (&pf, buffer, size);
      if (read == 0) {
        paged_file_close (&pf);
        open = FALSE;
        continue;
      }
      fwrite (buffer, read, 1, fd);
      len -= read;
    }
    fclose (fd);
  }

  if (open)
    paged_file_close (&pf);
  index_archive_free (&archive_index);

  return TRUE;
}

int
archive_dump_all (const char *path, const char *output)
{
  int ret;

  ret = archive_dump (path, "archive", output);
  if (ret && device_id_set)
    archive_dump (path, "archive2", output);

  return ret;
}

static int
chained_list_contains_string (ChainedList *list, const char *string)
{
  ChainedList *current = list;

  while (current != NULL) {
    if (strcmp (current->data, string) == 0)
      return 1;
    current = current->next;
  }

  return 0;
}

static void
populate_dirlist (ChainedList **dirs, ChainedList **files,
    const char *base, const char *subdir, DIR *fd)
{
  struct dirent *dirent = NULL;
  struct stat stat_buf;
  char path[1024];

  while (1) {
    dirent = readdir (fd);
    if (!dirent)
      break;
    if (strcmp (dirent->d_name, ".") == 0 ||
        strcmp (dirent->d_name, "..") == 0)
      continue;
    DBG ("Found %s : %s/%s\n", dirent->d_type == DT_DIR ? "directory" : "file" ,
        subdir, dirent->d_name);
    if (dirent->d_type == DT_DIR) {
      ArchiveDirectory *archive_dir = malloc (sizeof(ArchiveDirectory));
      DIR *dir_fd = NULL;

      snprintf (path, sizeof(path), "%s/%s/%s", base, subdir, dirent->d_name);
      snprintf (archive_dir->path, sizeof(archive_dir->path), "%s/%s",
          subdir, dirent->d_name);
      stat (path, &stat_buf);
      archive_dir->stat.mode = 0x41c0;
      archive_dir->stat.uid = 0;
      archive_dir->stat.gid = -1;
      archive_dir->stat.atime = stat_buf.st_atime;
      archive_dir->stat.mtime = stat_buf.st_mtime;
      archive_dir->stat.ctime = stat_buf.st_ctime;
      archive_dir->stat.file_size = 0x200;
      archive_dir->stat.block_size = 0x200;
      archive_dir->flags = 1;
      *dirs = chained_list_append (*dirs, archive_dir);

      dir_fd = opendir(path);
      if (!dir_fd)
        die ("Unable to open subdir\n");
      populate_dirlist (dirs, files, base, archive_dir->path, dir_fd);
      closedir (dir_fd);
    } else {
      ArchiveFile *archive_file = malloc (sizeof(ArchiveFile));

      snprintf (path, sizeof(path), "%s/%s/%s", base, subdir, dirent->d_name);
      stat (path, &stat_buf);
      snprintf (archive_file->path, sizeof(archive_file->path), "%s/%s",
          subdir, dirent->d_name);
      archive_file->stat.mode = 0x8180;
      archive_file->stat.uid = 0;
      archive_file->stat.gid = -1;
      archive_file->stat.atime = stat_buf.st_atime;
      archive_file->stat.mtime = stat_buf.st_mtime;
      archive_file->stat.ctime = stat_buf.st_ctime;
      archive_file->stat.file_size = stat_buf.st_size;
      archive_file->stat.block_size = 0x200;
      archive_file->flags = 0;
      *files = chained_list_append (*files, archive_file);
    }
  }
}

int
archive_add (const char *path, const char *game, int protected)
{
  ChainedList *list = NULL;
  char buffer[0x10000];
  ArchiveIndex archive_index;
  DatFileHeader dat_header;
  ArchiveHeader archive_header;
  ChainedList *dirs = NULL;
  ChainedList *files = NULL;
  PagedFile in = {0};
  PagedFile out = {0};
  u32 index = 0;
  int open = FALSE;
  FILE *fd = NULL;
  DIR *dir_fd = NULL;
  u32 total_file_size = 0;
  int new_file = TRUE;
  u8 key[0x10];
  u8 iv[0x10];
  u8 hmac[0x40];



  if (protected)
    archive_index.prefix = "archive2";
  else
    archive_index.prefix = "archive";

  dir_fd = opendir(game);
  if (!dir_fd)
    die ("Unable to open game dir\n");

  populate_dirlist (&dirs, &files, game, "", dir_fd);
  closedir (dir_fd);

  snprintf (buffer, sizeof(buffer), "%s/%s.dat", path, archive_index.prefix);
  if (!index_archive_read (&archive_index, buffer))
    die ("Unable to read index archive\n");

  for (list = dirs; list; list = list->next) {
    ArchiveDirectory *dir = list->data;

    if (strcmp (dir->path, "/dev_hdd0") == 0 ||
        chained_list_contains_string (archive_index.dirs, dir->path)) {
      free (dir);
      continue;
    }
    archive_index.dirs = chained_list_append (archive_index.dirs, dir);
  }
  chained_list_free (dirs);

  while (1) {
    snprintf (buffer, sizeof(buffer), "%s/%s_%02d.dat", path, archive_index.prefix, index);
    if (!file_exists (buffer)) {
      if (index == 0)
        break;
      if (!new_file)
        index--;
      break;
    }
    index++;
  }

  snprintf (buffer, sizeof(buffer), "%s/%s_%02d.dat", path, archive_index.prefix, index);
  if (file_exists (buffer)) {
    if (!archive_open (buffer, &in, &dat_header))
      die ("Couldn't open archive %d\n", index);

    if (paged_file_read (&in, &archive_header, sizeof(archive_header)) != sizeof(archive_header))
      die ("Couldn't read header\n");
    if (archive_header.id != archive_index.header.id)
      die ("Wrong archive ID\n");
    if (FROM_BE (32, archive_header.index) != index)
      die ("Wrong archive index\n");
    snprintf (buffer, sizeof(buffer), "%s/%s_%02d.tmp", path, archive_index.prefix, index);

    if (!paged_file_open (&out, buffer, FALSE))
      die ("Couldn't open output archive %d\n", index);

    dat_header.size = TO_LE (32, dat_header.size);
    dat_header.type = TO_BE (32, dat_header.type);
    if (paged_file_write (&out, &dat_header, sizeof(dat_header)) != sizeof(dat_header))
      die ("Couldn't write file dat header\n");
    total_file_size += sizeof(dat_header);

    if (!archive_gen_keys (&dat_header, key, iv, hmac))
      die ("Error generating keys\n");

    paged_file_flush (&out);
    paged_file_hash (&out, hmac);
    paged_file_crypt (&out, key, iv);

    if (paged_file_write (&out, &archive_header, sizeof(archive_header)) != sizeof(archive_header))
      die ("Couldn't write encrypted header\n");
    total_file_size += sizeof(archive_header);
    total_file_size += paged_file_splice (&out, &in, -1);
    if (total_file_size > 0xFFFFFE00)
      die ("Output file is too big\n");
    paged_file_close (&in);
    new_file = FALSE;
  } else {
    if (protected) {
      dat_header.size = TO_LE (32, 0x30);
      memset (dat_header.key_seed, 0, 0x14);
    } else {
      dat_header.size = TO_LE (32, 0x40);
      generate_random_key_seed (dat_header.key_seed);
    }
    dat_header.type = TO_BE (32, 0x05);
    memset (dat_header.padding, 0, 0x10);

    archive_header = archive_index.header;
    archive_header.index = TO_BE (32, index);

    snprintf (buffer, sizeof(buffer), "%s/%s_%02d.dat", path, archive_index.prefix, index);

    if (!paged_file_open (&out, buffer, FALSE))
      die ("Couldn't open output archive %d\n", index);

    if (paged_file_write (&out, &dat_header, sizeof(dat_header)) != sizeof(dat_header))
      die ("Couldn't write file dat header\n");
    total_file_size += sizeof(dat_header);

    if (!archive_gen_keys (&dat_header, key, iv, hmac))
      die ("Error generating keys\n");

    paged_file_flush (&out);
    paged_file_hash (&out, hmac);
    paged_file_crypt (&out, key, iv);

    if (paged_file_write (&out, &archive_header, sizeof(archive_header)) != sizeof(archive_header))
      die ("Couldn't write encrypted header\n");
    total_file_size += sizeof(archive_header);
    new_file = TRUE;
  }

  for (list = files; list; list = list->next) {
    ArchiveFile *file = list->data;

    if (chained_list_contains_string (archive_index.files, file->path)) {
      free (file);
      continue;
    }

    snprintf (buffer, 0x500, "%s/%s", game, file->path);
    fd = fopen (buffer, "rb");
    if (!fd)
      die ("Couldn't open input file : %s\n", buffer);

    while (1) {
      int read = fread (buffer, 1, sizeof(buffer), fd);
      if (read == 0)
        break;
      /* TODO : Must be able to exceed a file size of 0xFFFFFE00 by splitting */
      if (total_file_size + read > 0xFFFFFE00)
        die ("Output file is too big\n");
      paged_file_write (&out, buffer, read);
      total_file_size += read;
    }

    fclose (fd);
    archive_index.files = chained_list_append (archive_index.files, file);
  }
  paged_file_flush (&out);
  fd = out.fd;
  out.fd = NULL;
  paged_file_close (&out);

  fseek (fd, 8, SEEK_SET);
  fwrite (out.digest, 0x14, 1, fd);
  fclose (fd);

  if (!new_file) {
    snprintf (buffer, 0x500, "%s/%s_%02d.bak", path, archive_index.prefix, index);
    snprintf (buffer + 0x500, 0x500, "%s/%s_%02d.dat", path, archive_index.prefix, index);
    if (rename (buffer + 0x500, buffer) != 0)
      die ("File rename failed\n");
    snprintf (buffer, 0x500, "%s/%s_%02d.tmp", path, archive_index.prefix, index);
    if (rename (buffer, buffer + 0x500) != 0)
      die ("File rename failed\n");
  }
  snprintf (buffer, 0x500, "%s/%s.dat", path, archive_index.prefix);
  snprintf (buffer + 0x500, 0x500, "%s/%s.bak", path, archive_index.prefix);
  if (rename (buffer, buffer + 0x500) != 0)
    die ("File rename failed\n");
  if (!index_archive_write (&archive_index, buffer))
    die ("Unable to write index archive\n");

  index_archive_free (&archive_index);

  return TRUE;
}


int
archive_set_device_id (const u8 idps[0x10])
{
  memcpy (device_id, idps, 0x10);
  device_id_set = TRUE;
}