#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "make_ext4fs.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"

#include "../../external/yaffs2/yaffs2/utils/mkyaffs2image.h"
#include "../../external/yaffs2/yaffs2/utils/unyaffs.h"

#include "extendedcommands.h"
#include "nandroid.h"
#include "mounts.h"
#include "flashutils/flashutils.h"
#include "edify/expr.h"
#include <libgen.h>
#include "mtdutils/mtdutils.h"
#include "bmlutils/bmlutils.h"
#include "cutils/android_reboot.h"


int signature_check_enabled = 1;
int script_assert_enabled = 1;
static const char *SDCARD_UPDATE_FILE = "/sdcard/update.zip";
static const char *EMMC_UPDATE_FILE = "/emmc/update.zip";

void
toggle_signature_check()
{
    signature_check_enabled = !signature_check_enabled;
    ui_print("签名检测状态： %s\n", signature_check_enabled ? "开" : "关");
}

void toggle_script_asserts()
{
    script_assert_enabled = !script_assert_enabled;
    ui_print("高级脚本语言状态： %s\n", script_assert_enabled ? "开" : "关");
}

int install_zip(const char* packagefilepath)
{
    ui_print("\n-- 正在安装： %s\n", packagefilepath);
    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }
    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
        ui_print("安装终止\n");
        return 1;
    }
    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
    ui_print("\n安装完毕\n");
    return 0;
}

char* INSTALL_MENU_ITEMS[] = {  "从外置SD卡选择zip刷机包",
                                "从内置SD卡选择zip刷机包",
                                "选择外置SD卡中的update.zip",
                                "选择内置SD卡中的update.zip",
                                "切换签名检测状态",
                                "切换高级脚本语言",
                                NULL };
#define ITEM_CHOOSE_ZIP       0
#define ITEM_CHOOSE_ZIP_INT   1
#define ITEM_APPLY_SDCARD     2
#define ITEM_APPLY_EMMC       3
#define ITEM_SIG_CHECK        4
#define ITEM_ASSERTS          5

void show_install_update_menu()
{
    static char* headers[] = {  "从SD卡选择zip刷机包",
                                "",
                                NULL
    };
    
    char *install_menu_items[7];
    int i;
    for (i = 0; i < 7; i++) {
    	install_menu_items[i] = INSTALL_MENU_ITEMS[i];
    }

    if (volume_for_path("/emmc") == NULL) {
    	// reorder
    	install_menu_items[0] = INSTALL_MENU_ITEMS[ITEM_CHOOSE_ZIP];
    	install_menu_items[1] = INSTALL_MENU_ITEMS[ITEM_APPLY_SDCARD];
    	install_menu_items[2] = INSTALL_MENU_ITEMS[ITEM_SIG_CHECK];
    	install_menu_items[3] = INSTALL_MENU_ITEMS[ITEM_ASSERTS];
    	install_menu_items[4] = NULL;
    }

    for (;;)
    {
        //int chosen_item = get_menu_selection(headers, INSTALL_MENU_ITEMS, 0, 0);
        int chosen_item = get_menu_selection(headers, install_menu_items, 0, 0);
        if (volume_for_path("/emmc") == NULL) {
			switch (chosen_item) {
				case 0:
					chosen_item = ITEM_CHOOSE_ZIP;
					break;
				case 1:
					chosen_item = ITEM_APPLY_SDCARD;
					break;
				case 2:
					chosen_item = ITEM_SIG_CHECK;
					break;
				case 3:
					chosen_item = ITEM_ASSERTS;
					break;
				default:
					return;
			}
		}
        switch (chosen_item)
        {
            case ITEM_ASSERTS:
                toggle_script_asserts();
                break;
            case ITEM_SIG_CHECK:
                toggle_signature_check();
                break;
            case ITEM_APPLY_SDCARD:
            {
                if (confirm_selection("确认安装？", "是的，确认安装/sdcard/update.zip"))
                    install_zip(SDCARD_UPDATE_FILE);
                break;
            }
            case ITEM_APPLY_EMMC:
            {
                if (confirm_selection("确认安装？", "是的，确认安装/emmc/update.zip"))
                    install_zip(EMMC_UPDATE_FILE);
                break;
            }
            case ITEM_CHOOSE_ZIP:
                show_choose_zip_menu("/sdcard/");
                break;
            case ITEM_CHOOSE_ZIP_INT:
                show_choose_zip_menu("/emmc/");
                break;
            default:
                return;
        }
    }
}

void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("错误！无法打开目录\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }

    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("未找到任何文件\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {
            int chosen_item = get_menu_selection(headers, list, 0, 0);
            if (chosen_item == GO_BACK)
                break;
            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

void show_choose_zip_menu(const char *mount_point)
{
    if (ensure_path_mounted(mount_point) != 0) {
        LOGE ("Can't mount %s\n", mount_point);
        return;
    }

    static char* headers[] = {  "选择zip刷机包",
                                "",
                                NULL
    };

    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    static char* confirm_install  = "确认安装？";
    static char confirm[PATH_MAX];
    sprintf(confirm, "是的，确认安装 %s", basename(file));
    if (confirm_selection(confirm_install, confirm))
        install_zip(file);
}

void show_nandroid_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE("Can't mount %s\n", path);
        return;
    }

    static char* headers[] = {  "选择需要还原的备份",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

    if (confirm_selection("确认还原？", "是的，确认还原"))
        nandroid_restore(file, 1, 1, 1, 1, 1, 0);
}

#ifndef BOARD_UMS_LUNFILE
#define BOARD_UMS_LUNFILE	"/sys/devices/platform/usb_mass_storage/lun0/file"
#endif

void show_mount_usb_storage_menu()
{
#ifndef BOARD_UMS_2ND_LUNFILE
    Volume *vol = volume_for_path("/emmc");
    if(vol) {// board has internal sdcard
		static char* select_sd_list[] = { "挂载外置SD卡",
										  "挂载内置SD卡",
										  NULL
		};
		static char* select_sd_headers[] = { "选择从电脑读取的SD卡",
											 "",
											 NULL
		};

		int chosen_item = get_menu_selection(select_sd_headers, select_sd_list, 0, 0);
		switch (chosen_item) {
			case 0:
				vol = volume_for_path("/sdcard");
				break;
			case 1:
				vol = volume_for_path("/emmc");
				break;
			default:
				return;
		}
    } else
    	vol = volume_for_path("/sdcard");

    int fd;
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)\n", strerror(errno));
        return -1;
    }
    if ((write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("Unable to write to ums lunfile (%s)\n", strerror(errno));
        close(fd);
        return -1;
    }
#else// defined(BOARD_UMS_2ND_LUNFILE)
	int mount_sdcard_failed = 0;
    int fd;
    Volume *vol = volume_for_path("/sdcard");
    //Volume *vol = volume_for_path("/emmc");
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)\n", strerror(errno));
        //return -1;
        mount_sdcard_failed = 1;
    }
    if (mount_sdcard_failed == 0 && (write(fd, vol->device, strlen(vol->device)) < 0) &&
        (!vol->device2 || (write(fd, vol->device, strlen(vol->device2)) < 0))) {
        LOGE("Unable to write to ums lunfile (%s)\n", strerror(errno));
        close(fd);
        //return -1;
        mount_sdcard_failed = 1;
    }
    if (mount_sdcard_failed == 1)
    	ui_print("挂载外置SD卡失败\n");
    else
    	ui_print("成功挂载外置SD卡\n");

    int mount_emmec_failed = 0;
    int fd2;
    Volume *vol2 = volume_for_path("/emmc");
    if ((fd2 = open(BOARD_UMS_2ND_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums 2nd lunfile (%s)\n", strerror(errno));
        //return -1;
        mount_emmec_failed = 1;
    }
    if (mount_emmec_failed == 0 && (write(fd2, vol2->device, strlen(vol2->device)) < 0) &&
        (!vol2->device2 || (write(fd2, vol2->device, strlen(vol2->device2)) < 0))) {
        LOGE("Unable to write to ums 2nd lunfile (%s)\n", strerror(errno));
        close(fd2);
        //return -1;
        mount_emmec_failed = 1;
    }
    if (mount_sdcard_failed == 1)
    	ui_print("挂载内置SD卡失败\n");
    else
    	ui_print("成功挂载内置SD卡\n");

    if (mount_sdcard_failed == 1 && mount_sdcard_failed == 1)
    	return -1;
#endif

    static char* headers[] = {  "通过USB连接电脑读取SD卡",
                                "离开此菜单电脑就无法读取SD卡",
                                "",
                                NULL
    };

    static char* list[] = { "卸载", NULL };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    int unmount_failed = 0;
    if ((fd = open(BOARD_UMS_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums lunfile (%s)\n", strerror(errno));
        //return -1;
        unmount_failed = 1;
    }
    char ch = 0;
    if (unmount_failed == 0 && write(fd, &ch, 1) < 0) {
        LOGE("Unable to write to ums lunfile (%s)\n", strerror(errno));
        close(fd);
        //return -1;
        unmount_failed = 1;
    }

#ifdef BOARD_UMS_2ND_LUNFILE
    int unmount_failed2 = 0;
    if ((fd2 = open(BOARD_UMS_2ND_LUNFILE, O_WRONLY)) < 0) {
        LOGE("Unable to open ums 2nd lunfile (%s)\n", strerror(errno));
        //return -1;
        unmount_failed2 = 1;
    }
    if (unmount_failed2 == 0 && write(fd2, &ch, 1) < 0) {
        LOGE("Unable to write to ums 2nd lunfile (%s)\n", strerror(errno));
        close(fd2);
        //return -1;
        unmount_failed2 = 1;
    }
    if (unmount_failed2 == 1)
    	return -1;
#endif

    if (unmount_failed == 1)
    	return -1;
}

int confirm_selection(const char* title, const char* confirm)
{
    struct stat info;
    if (0 == stat("/sdcard/clockworkmod/.no_confirm", &info))
        return 1;

    char* confirm_headers[]  = {  title, "（此操作不可撤销）", "", NULL };
    char* items[] = { "否",
                      confirm, //" Yes -- wipe partition",   // [1]
                      NULL };

    int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
    return chosen_item == 1;
}

#define MKE2FS_BIN      "/sbin/mke2fs"
#define TUNE2FS_BIN     "/sbin/tune2fs"
#define E2FSCK_BIN      "/sbin/e2fsck"

int format_device(const char *device, const char *path, const char *fs_type) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // no /sdcard? let's assume /data/media
        if (strstr(path, "/sdcard") == path && is_data_media()) {
            return format_unknown_device(NULL, path, NULL);
        }
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") == 0)
            return -1;
        LOGE("unknown volume \"%s\"\n", path);
        return -1;
    }
    if (strstr(path, "/data") == path && volume_for_path("/sdcard") == NULL && is_data_media()) {
        return format_unknown_device(NULL, path, NULL);
    }
    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
        LOGE("can't format_volume \"%s\"", path);
        return -1;
    }

    if (strcmp(fs_type, "rfs") == 0) {
        if (ensure_path_unmounted(path) != 0) {
            LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
            return -1;
        }
        if (0 != format_rfs_device(device, path)) {
            LOGE("format_volume: format_rfs_device failed on %s\n", device);
            return -1;
        }
        return 0;
    }
 
    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) -1) {
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
            LOGW("format_volume: can't close MTD \"%s\"\n",device);
            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        int length = 0;
        if (strcmp(v->fs_type, "ext4") == 0) {
            // Our desired filesystem matches the one in fstab, respect v->length
            length = v->length;
        }
        reset_ext4fs_info();
        int result = make_ext4fs(device, length);
        if (result != 0) {
            LOGE("format_volume: make_extf4fs failed on %s\n", device);
            return -1;
        }
        return 0;
    }

    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type)
{
    LOGI("Formatting unknown device.\n");

    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext"))
    {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->device, &st))
        {
            ui_print("未发现sd-ext分区（App2SD+），跳过格式化sd-ext\n");
            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
            LOGI("Formatting ext3 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
            LOGI("Formatting ext2 device.\n");
            if (0 != ensure_path_unmounted(path)) {
                LOGE("Error while unmounting %s.\n", path);
                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path))
    {
        ui_print("挂载 %s 出错\n", path);
        ui_print("跳过格式化...\n");
        return 0;
    }

    static char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
    }
    else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

//#define MOUNTABLE_COUNT 5
//#define DEVICE_COUNT 4
//#define MMC_COUNT 2

typedef struct {
    char mount[255];
    char unmount[255];
    Volume* v;
} MountMenuEntry;

typedef struct {
    char txt[255];
    Volume* v;
} FormatMenuEntry;

int is_safe_to_format(char* name)
{
    char str[255];
    char* partition;
    property_get("ro.cwm.forbid_format", str, "/misc,/radio,/bootloader,/recovery,/efs");
    //property_get("ro.cwm.forbid_format", str, "/radio,/bootloader,/recovery,/efs");

    partition = strtok(str, ", ");
    while (partition != NULL) {
        if (strcmp(name, partition) == 0) {
            return 0;
        }
        partition = strtok(NULL, ", ");
    }

    return 1;
}

void show_partition_menu()
{
    static char* headers[] = {  "挂载菜单",
                                "",
                                NULL
    };

    static MountMenuEntry* mount_menue = NULL;
    static FormatMenuEntry* format_menue = NULL;

    typedef char* string;

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    Volume* device_volumes;

    num_volumes = get_num_volumes();
    device_volumes = get_device_volumes();

    string options[255];

    if(!device_volumes)
		    return;

		mountable_volumes = 0;
		formatable_volumes = 0;

		mount_menue = malloc(num_volumes * sizeof(MountMenuEntry));
		format_menue = malloc(num_volumes * sizeof(FormatMenuEntry));

		for (i = 0; i < num_volumes; ++i) {
  			Volume* v = &device_volumes[i];
  			if(strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) != 0 && strcmp("emmc", v->fs_type) != 0 && strcmp("bml", v->fs_type) != 0) {
				sprintf(&mount_menue[mountable_volumes].mount, "挂载 %s", v->mount_point);
				sprintf(&mount_menue[mountable_volumes].unmount, "卸载 %s", v->mount_point);
				mount_menue[mountable_volumes].v = &device_volumes[i];
				++mountable_volumes;
				if (is_safe_to_format(v->mount_point)) {
					sprintf(&format_menue[formatable_volumes].txt, "格式化 %s", v->mount_point);
					format_menue[formatable_volumes].v = &device_volumes[i];
					++formatable_volumes;
				}
  		    }
  		    else if (strcmp("ramdisk", v->fs_type) != 0 && strcmp("mtd", v->fs_type) == 0 && is_safe_to_format(v->mount_point))
  		    {
				sprintf(&format_menue[formatable_volumes].txt, "格式化 %s", v->mount_point);
				format_menue[formatable_volumes].v = &device_volumes[i];
				++formatable_volumes;
  			}
		}


    static char* confirm_format  = "确认格式化？";
    static char* confirm = "是的，确认格式化";
    char confirm_string[255];

    for (;;)
    {
    		for (i = 0; i < mountable_volumes; i++)
    		{
    			MountMenuEntry* e = &mount_menue[i];
    			Volume* v = e->v;
    			if(is_path_mounted(v->mount_point))
    				options[i] = e->unmount;
    			else
    				options[i] = e->mount;
    		}

    		for (i = 0; i < formatable_volumes; i++)
    		{
    			FormatMenuEntry* e = &format_menue[i];

    			options[mountable_volumes+i] = e->txt;
    		}

        if (!is_data_media()) {
          options[mountable_volumes + formatable_volumes] = "挂载USB存储";
          options[mountable_volumes + formatable_volumes + 1] = NULL;
        }
        else {
          options[mountable_volumes + formatable_volumes] = NULL;
        }

        int chosen_item = get_menu_selection(headers, &options, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        if (chosen_item == (mountable_volumes + formatable_volumes)) {
            show_mount_usb_storage_menu();
        }
        else if (chosen_item < mountable_volumes) {
			      MountMenuEntry* e = &mount_menue[chosen_item];
            Volume* v = e->v;

            if (is_path_mounted(v->mount_point))
            {
                if (0 != ensure_path_unmounted(v->mount_point))
                    ui_print("卸载 %s 出错\n", v->mount_point);
            }
            else
            {
                if (0 != ensure_path_mounted(v->mount_point))
                    ui_print("挂载 %s 出错\n",  v->mount_point);
            }
        }
        else if (chosen_item < (mountable_volumes + formatable_volumes))
        {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menue[chosen_item];
            Volume* v = e->v;

            sprintf(confirm_string, "%s - %s", v->mount_point, confirm_format);

            if (!confirm_selection(confirm_string, confirm))
                continue;
            ui_print("正在格式化 %s...\n", v->mount_point);
            if (0 != format_volume(v->mount_point))
                ui_print("格式化 %s 出错\n", v->mount_point);
            else
                ui_print("格式化完毕\n");
        }
    }

    free(mount_menue);
    free(format_menue);
}

void show_nandroid_advanced_restore_menu(const char* path)
{
    if (ensure_path_mounted(path) != 0) {
        LOGE ("Can't mount sdcard\n");
        return;
    }

    static char* advancedheaders[] = {  "选择需要还原的备份",
                                "",
                                "首先选择一个备份，然后",
                                "进入选择需要还原的数据",
                                "",
                                NULL
    };

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = {  "选择需要还原的数据",
                                "",
                                NULL
    };

    static char* list[] = { "还原引导分区 [boot]",
                            "还原系统分区 [system]",
                            "还原用户数据 [data]",
                            "还原缓存     [cache]",
                            "还原App2SD+  [sd-ext]",
                            "还原WiMax    [wimax]",
                            NULL
    };
    
    if (0 != get_partition_device("wimax", tmp)) {
        // disable wimax restore option
        list[5] = NULL;
    }

    static char* confirm_restore  = "确认还原？";

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            if (confirm_selection(confirm_restore, "是的，还原引导分区 [boot]"))
                nandroid_restore(file, 1, 0, 0, 0, 0, 0);
            break;
        case 1:
            if (confirm_selection(confirm_restore, "是的，还原系统分区 [system]"))
                nandroid_restore(file, 0, 1, 0, 0, 0, 0);
            break;
        case 2:
            if (confirm_selection(confirm_restore, "是的，还原用户数据 [data]"))
                nandroid_restore(file, 0, 0, 1, 0, 0, 0);
            break;
        case 3:
            if (confirm_selection(confirm_restore, "是的，还原缓存 [cache]"))
                nandroid_restore(file, 0, 0, 0, 1, 0, 0);
            break;
        case 4:
        {
            struct stat st;
            Volume *vol = volume_for_path("/sd-ext");
            if (vol == NULL || 0 != stat(vol->device, &st)) {
                ui_print("未发现sd-ext分区\n");
                break;
            }

            if (confirm_selection(confirm_restore, "是的，还原App2SD+ [sd-ext]"))
                nandroid_restore(file, 0, 0, 0, 0, 1, 0);
            break;
        }
        case 5:
            if (confirm_selection(confirm_restore, "是的，还原WiMax [wimax]"))
                nandroid_restore(file, 0, 0, 0, 0, 0, 1);
            break;
    }
}

void show_nandroid_menu()
{
    static char* headers[] = {  "Nandroid",
                                "",
                                NULL
    };

    static char* list[] = { "备份到外置SD卡",
                            "还原从外置SD卡",
                            "高级还原从外置SD卡",
                            "备份到内置SD卡",
                            "还原从内置SD卡",
                            "高级还原从内置SD卡",
                            NULL
    };
//    static char* list[] = { "备份到SD卡",
//                            "还原从SD卡",
//                            "高级还原从SD卡",
//                            "备份到内置SD卡",
//                            "还原从内置SD卡",
//                            "高级还原从内置SD卡",
//                            NULL
//    };

    if (volume_for_path("/emmc") == NULL || volume_for_path("/sdcard") == NULL && is_data_media())
        list[3] = NULL;

    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item)
    {
        case 0:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/sdcard/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/sdcard/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 1:
            show_nandroid_restore_menu("/sdcard");
            break;
        case 2:
            show_nandroid_advanced_restore_menu("/sdcard");
            break;
        case 3:
            {
                char backup_path[PATH_MAX];
                time_t t = time(NULL);
                struct tm *tmp = localtime(&t);
                if (tmp == NULL)
                {
                    struct timeval tp;
                    gettimeofday(&tp, NULL);
                    sprintf(backup_path, "/emmc/clockworkmod/backup/%d", tp.tv_sec);
                }
                else
                {
                    strftime(backup_path, sizeof(backup_path), "/emmc/clockworkmod/backup/%F.%H.%M.%S", tmp);
                }
                nandroid_backup(backup_path);
            }
            break;
        case 4:
            show_nandroid_restore_menu("/emmc");
            break;
        case 5:
            show_nandroid_advanced_restore_menu("/emmc");
            break;
    }
}

void wipe_battery_stats()
{
    ensure_path_mounted("/data");
    remove("/data/system/batterystats.bin");
    ensure_path_unmounted("/data");
    ui_print("电池数据清空完毕\n");
}

void show_advanced_menu()
{
    static char* headers[] = {  "高级调试菜单",
                                "",
                                NULL
    };

    static char* list[] = { "重新启动Recovery",
                            "清空Dalvik缓存",
                            "清空电池数据",
                            "报告错误",
                            "键位测试",
                            "显示记录",
                            "对外置SD卡分区",//"对SD卡分区",
#ifdef BOARD_HAS_SDCARD_INTERNAL
                            "对内置SD卡分区",
#endif
                            "修复权限",
                            NULL
    };

    for (;;)
    {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK)
            break;
        switch (chosen_item)
        {
            case 0:
            {
                android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
                break;
            }
            case 1:
            {
                if (0 != ensure_path_mounted("/data"))
                    break;
                ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
                if (confirm_selection( "确认清空？", "是的，清空Dalvik缓存")) {
                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("Dalvik缓存清空完毕\n");
                }
                ensure_path_unmounted("/data");
                break;
            }
            case 2:
            {
                if (confirm_selection( "确认清空？", "是的，清空电池状态"))
                    wipe_battery_stats();
                break;
            }
            case 3:
                handle_failure(1);
                break;
            case 4:
            {
                ui_print("正显示键位代码...\n");
                ui_print("按返回结束调试\n");
                int key;
                int action;
                do
                {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
                    ui_print("键位代码: %d\n", key);
                }
                while (action != GO_BACK);
                break;
            }
            case 5:
            {
                ui_printlogtail(12);
                break;
            }
            case 6:
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* ext_fs[] = { "ext2",
                                          "ext3",
                                          "ext4",
                                          NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Ext分区大小", "", NULL };
                static char* ext_fs_headers[] = { "Ext分区类型", "", NULL };
                static char* swap_headers[] = { "Swap分区大小", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int ext_fs_selected = get_menu_selection(ext_fs_headers, ext_fs, 0, 0);
                if (ext_fs_selected == GO_BACK)
                    continue;

                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/sdcard");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], ext_fs[ext_fs_selected]);
                ui_print("正对外置SD卡分区，请稍等...\n");
                if (0 == __system(cmd))
                    ui_print("分区完毕\n");
                else
                    ui_print("分区时发生错误！详细信息请查看/tmp/recovery.log\n");
                break;
            }
            case 7:
#ifdef BOARD_HAS_SDCARD_INTERNAL
            {
                static char* ext_sizes[] = { "128M",
                                             "256M",
                                             "512M",
                                             "1024M",
                                             "2048M",
                                             "4096M",
                                             NULL };

                static char* ext_fs[] = { "ext2",
                                          "ext3",
                                          "ext4",
                                          NULL };

                static char* swap_sizes[] = { "0M",
                                              "32M",
                                              "64M",
                                              "128M",
                                              "256M",
                                              NULL };

                static char* ext_headers[] = { "Data分区大小", "", NULL };
                static char* ext_fs_headers[] = { "Ext分区类型", "", NULL };
                static char* swap_headers[] = { "Swap分区大小", "", NULL };

                int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
                if (ext_size == GO_BACK)
                    continue;

                int ext_fs_selected = get_menu_selection(ext_fs_headers, ext_fs, 0, 0);
                if (ext_fs_selected == GO_BACK)
                    continue;

                int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
                if (swap_size == GO_BACK)
                    continue;

                char sddevice[256];
                Volume *vol = volume_for_path("/emmc");
                strcpy(sddevice, vol->device);
                // we only want the mmcblk, not the partition
                sddevice[strlen("/dev/block/mmcblkX")] = NULL;
                char cmd[PATH_MAX];
                setenv("SDPATH", sddevice, 1);
                sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], ext_fs[ext_fs_selected]);
                ui_print("正对内置SD卡分区，请稍等...\n");
                if (0 == __system(cmd))
                    ui_print("分区完毕\n");
                else
                    ui_print("分区时发生错误！详细信息请查看/tmp/recovery.log\n");
                break;
            }
            case 8:
#endif
            {
                ensure_path_mounted("/system");
                ensure_path_mounted("/data");
                ui_print("正在修复权限...\n");
                __system("fix_permissions");
                ui_print("修复完毕\n");
                break;
            }
        }
    }
}

void write_fstab_root(char *path, FILE *file)
{
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGW("Unable to get recovery.fstab info for %s during fstab generation!\n", path);
        return;
    }

    char device[200];
    if (vol->device[0] != '/')
        get_partition_device(vol->device, device);
    else
        strcpy(device, vol->device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

void create_fstab()
{
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
        LOGW("Unable to create /etc/fstab!\n");
        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
         write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    write_fstab_root("/datadata", file);
    write_fstab_root("/emmc", file);
    write_fstab_root("/system", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    fclose(file);
    LOGI("Completed outputting fstab.\n");
}

int bml_check_volume(const char *path) {
    ui_print("检查 %s...\n", path);
    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }
    
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
        LOGE("Unable process volume! Skipping...\n");
        return 0;
    }
    
    ui_print("%s 可能是rfs分区，检查中...\n", path);
    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;

    // dead code.
    if (device_flash_type() != BML)
        return;

    ui_print("正在检查ext4分区...\n");
    int ret = 0;
    ret = bml_check_volume("/system");
    ret |= bml_check_volume("/data");
    if (has_datadata())
        ret |= bml_check_volume("/datadata");
    ret |= bml_check_volume("/cache");
    
    if (ret == 0) {
        ui_print("检查完毕\n");
        return;
    }
    
    char backup_path[PATH_MAX];
    time_t t = time(NULL);
    char backup_name[PATH_MAX];
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sprintf(backup_name, "before-ext4-convert-%d", tp.tv_sec);
    sprintf(backup_path, "/sdcard/clockworkmod/backup/%s", backup_name);

    ui_set_show_text(1);
    ui_print("文件系统类型需要转成ext4\n");
    ui_print("一次备份再还原将完成这项工作\n");
    ui_print("此备份名称为：%s\n", backup_name);
    ui_print("如果期间发生任何问题，请再次尝试还原它！\n");

    nandroid_backup(backup_path);
    nandroid_restore(backup_path, 1, 1, 1, 1, 1, 0);
    ui_set_show_text(0);
}

void handle_failure(int ret)
{
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted("/sdcard"))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
    ui_print("复制记录文件/tmp/recovery.log到/sdcard/clockworkmod/recovery.log，请用ROM Manager反馈问题\n");
}

int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    int result;
    result = scan_mounted_volumes();
    if (result < 0) {
        LOGE("failed to scan mounted volumes\n");
        return 0;
    }

    const MountedVolume* mv =
        find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}

#ifndef BOARD_USES_RECOVERY_CHARGEMODE
void handle_chargemode() {
    const char* filename = "/proc/cmdline";
    struct stat file_info;
    if (0 != stat(filename, &file_info))
        return;

    int file_len = file_info.st_size;
    char* file_data = (char*)malloc(file_len + 1);
    FILE *file = fopen(filename, "rb");
    if (file == NULL)
        return;
    fread(file_data, file_len, 1, file);
    // supposedly not necessary, but let's be safe.
    file_data[file_len] = '\0';
    fclose(file);

    if (strstr(file_data, "androidboot.mode=offmode_charging") != NULL)
        reboot(RB_POWER_OFF);
}
#endif
