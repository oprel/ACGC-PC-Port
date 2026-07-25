#ifndef PC_SAVE_LOCATION_H
#define PC_SAVE_LOCATION_H

#ifdef __cplusplus
extern "C" {
#endif

void pc_save_location_init(void);

/* Either default save directory or the one set in settings.ini */
const char* pc_save_root(void);

/* Create .LOCK file in save folder to indicate this save file is currently being used. For when multiple people have access to the same save file.*/
int pc_save_lock_acquire(const char* dir);

/* Deletes .LOCK file when you are done playing */
void pc_save_lock_release(void);

/* Update the timestamp on the current save lock. Can be called every frame, but only executes once per minute. */
void pc_save_lock_heartbeat(void);

/* True if save_location is an http(s) URL*/
int pc_save_is_remote(void);

/* Download/upload the Card A GCI to the save server */
int pc_save_http_load_to_file(const char* path);
int pc_save_http_save_from_file(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PC_SAVE_LOCATION_H */
