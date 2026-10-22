#pragma once
/* Export the emulated machine's RAM files (see vt_export.c). */

typedef void *vt_file_t;

/* provided by the host: create/overwrite a file, write, close, make a folder
   (an existing folder is fine). create returns 1 on success. */
int  vt_host_create(const char *path, vt_file_t *f);
int  vt_host_write(vt_file_t f, const void *data, int len);   /* bytes written */
void vt_host_close(vt_file_t f);
void vt_host_mkdir(const char *path);

/* called once per file written: name ("ROG.BA"), ok = 1 if written */
typedef void (*vt_export_report_fn)(const char *name, int ok);

/* write every .BA/.DO/.CO in the current RAM directory into dir;
   returns the number of RAM files exported */
int vt_export_files(const char *dir, vt_export_report_fn report);
