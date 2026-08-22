#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ICON_150_WIDTH 150u
#define ICON_150_HEIGHT 150u
#define ICON_150_BPP 4u
#define ICON_150_DATA_SIZE ((ICON_150_WIDTH * ICON_150_HEIGHT) / 2u)

typedef enum {
    ICON_150_ABOUT,
    ICON_150_ADD,
    ICON_150_BINOCULARS,
    ICON_150_BOOKMARK,
    ICON_150_BOOKMARK_RIBBON,
    ICON_150_CANCEL,
    ICON_150_CANCEL_ALT,
    ICON_150_CHECKED,
    ICON_150_CHECKED_BOX,
    ICON_150_CHECKMARK,
    ICON_150_CLOCK,
    ICON_150_DELETE,
    ICON_150_DELETE_ALT,
    ICON_150_DIALOG,
    ICON_150_DONE,
    ICON_150_DOWNLOAD_FROM_CLOUD,
    ICON_150_EDIT,
    ICON_150_FILE,
    ICON_150_FILE_ALT,
    ICON_150_FOLDER,
    ICON_150_HOME,
    ICON_150_IDEA,
    ICON_150_IMAGE_FILE,
    ICON_150_INFO,
    ICON_150_LOCK,
    ICON_150_MENU,
    ICON_150_MUSIC,
    ICON_150_NEWS,
    ICON_150_OPEN_FOLDER,
    ICON_150_PICTURE,
    ICON_150_REFRESH,
    ICON_150_REMOVE,
    ICON_150_RESTART,
    ICON_150_SEARCH,
    ICON_150_SETTINGS,
    ICON_150_SHARE,
    ICON_150_SUN,
    ICON_150_SUPPORT,
    ICON_150_SYNC,
    ICON_150_TOOLBOX,
    ICON_150_TRASH,
    ICON_150_UNLOCK,
    ICON_150_UPLOAD_TO_CLOUD,
    ICON_150_COUNT
} icon_150_id_t;

typedef struct {
    const char *name;
    uint16_t width;
    uint16_t height;
    const uint8_t *data;
} icon_150_t;

extern const uint8_t icon_150_about_data[11250];
extern const uint8_t icon_150_add_data[11250];
extern const uint8_t icon_150_binoculars_data[11250];
extern const uint8_t icon_150_bookmark_data[11250];
extern const uint8_t icon_150_bookmark_ribbon_data[11250];
extern const uint8_t icon_150_cancel_data[11250];
extern const uint8_t icon_150_cancel_alt_data[11250];
extern const uint8_t icon_150_checked_data[11250];
extern const uint8_t icon_150_checked_box_data[11250];
extern const uint8_t icon_150_checkmark_data[11250];
extern const uint8_t icon_150_clock_data[11250];
extern const uint8_t icon_150_delete_data[11250];
extern const uint8_t icon_150_delete_alt_data[11250];
extern const uint8_t icon_150_dialog_data[11250];
extern const uint8_t icon_150_done_data[11250];
extern const uint8_t icon_150_download_from_cloud_data[11250];
extern const uint8_t icon_150_edit_data[11250];
extern const uint8_t icon_150_file_data[11250];
extern const uint8_t icon_150_file_alt_data[11250];
extern const uint8_t icon_150_folder_data[11250];
extern const uint8_t icon_150_home_data[11250];
extern const uint8_t icon_150_idea_data[11250];
extern const uint8_t icon_150_image_file_data[11250];
extern const uint8_t icon_150_info_data[11250];
extern const uint8_t icon_150_lock_data[11250];
extern const uint8_t icon_150_menu_data[11250];
extern const uint8_t icon_150_music_data[11250];
extern const uint8_t icon_150_news_data[11250];
extern const uint8_t icon_150_open_folder_data[11250];
extern const uint8_t icon_150_picture_data[11250];
extern const uint8_t icon_150_refresh_data[11250];
extern const uint8_t icon_150_remove_data[11250];
extern const uint8_t icon_150_restart_data[11250];
extern const uint8_t icon_150_search_data[11250];
extern const uint8_t icon_150_settings_data[11250];
extern const uint8_t icon_150_share_data[11250];
extern const uint8_t icon_150_sun_data[11250];
extern const uint8_t icon_150_support_data[11250];
extern const uint8_t icon_150_sync_data[11250];
extern const uint8_t icon_150_toolbox_data[11250];
extern const uint8_t icon_150_trash_data[11250];
extern const uint8_t icon_150_unlock_data[11250];
extern const uint8_t icon_150_upload_to_cloud_data[11250];

extern const icon_150_t icon_150_assets[ICON_150_COUNT];

#ifdef __cplusplus
}
#endif
