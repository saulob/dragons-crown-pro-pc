/* Focused tests of the save-data mounts and the Japanese startup save hook. The save code is compiled in directly and
 * called through its export names, as the guest calls it. No game files, no window. */
#include "runtime_savedata.c"
#include "startup_save_hook.h"
#include <stdlib.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef DCP_SAVEDATA_TEST_ROOT
#define DCP_SAVEDATA_TEST_ROOT "savedata-tests"
#endif

static char user_dir[1024];
static char mounted_host[1024];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) {
    (void)guest;
    snprintf(mounted_host, sizeof(mounted_host), "%s", host);
    return 0;
}
void runtime_file_unmount(const char *guest) { (void)guest; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}
#ifdef _WIN32
int compat_remove_tree(const char *path) { (void)path; return 0; }
#endif

typedef int32_t (ABI *InitializeFn)(const void *);
typedef int32_t (ABI *TerminateFn)(void);
typedef int32_t (ABI *Mount2Fn)(const Mount2 *, MountResult *);
typedef int32_t (ABI *UmountFn)(const MountPoint *);

#define GUEST_CREATE_MODE (MODE_CREATE | MODE_COPY_ICON | 2)

static int checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #condition); } } while (0)

static uintptr_t guest(const char *name) { return runtime_savedata_resolve(name); }

static void wipe(const char *path) {
    struct stat st;
    if (stat(path, &st)) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    DIR *d = opendir(path);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1100];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        wipe(child);
    }
    if (d) closedir(d);
    rmdir(path);
}
static void tree_mkdir(const char *path) {
    char buffer[1100];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 1; *p; ++p) if (*p == '/') { *p = 0; mkdir(buffer, 0755); *p = '/'; }
    mkdir(buffer, 0755);
}
static int is_dir(const char *path) { struct stat st; return !stat(path, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *path) { struct stat st; return !stat(path, &st) && S_ISREG(st.st_mode); }
static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}
static int read_text(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return 1;
}

static char root_dir[1024];
static void save_dir_path(char *out, size_t size, const char *title, const char *dir) {
    snprintf(out, size, "%s/savedata/1/%s/%s", user_dir, title, dir);
}
static void begin(const char *name, const char *title) {
    snprintf(root_dir, sizeof(root_dir), "%s/%s", DCP_SAVEDATA_TEST_ROOT, name);
    wipe(root_dir);
    tree_mkdir(root_dir);
    snprintf(user_dir, sizeof(user_dir), "%s/user", root_dir);
    tree_mkdir(user_dir);
    runtime_savedata_configure(title);
    ((InitializeFn)guest("sceSaveDataInitialize3"))(NULL);
}
static void end(void) { ((TerminateFn)guest("sceSaveDataTerminate"))(); }

static int32_t mount_save(int32_t user, const char *dir_name, uint32_t mode, MountResult *out) {
    DirName dir;
    memset(&dir, 0, sizeof(dir));
    snprintf(dir.data, sizeof(dir.data), "%s", dir_name);
    Mount2 request;
    memset(&request, 0, sizeof(request));
    request.user = user;
    request.dir = &dir;
    request.blocks = 32768;
    request.mode = mode;
    return ((Mount2Fn)guest("sceSaveDataMount2"))(&request, out);
}
static void unmount_save(const MountResult *mounted) { ((UmountFn)guest("sceSaveDataUmount"))(&mounted->point); }

static void test_missing_save_is_not_found_for_every_title(void) {
    const char *titles[] = {"CUSA07001", "CUSA10454", "CUSA10209"};
    for (size_t i = 0; i < sizeof(titles) / sizeof(*titles); ++i) {
        begin(titles[i], titles[i]);
        char save[1200];
        save_dir_path(save, sizeof(save), titles[i], "SAVEDATA00");
        MountResult mounted;
        CHECK(mount_save(1, "SAVEDATA00", MODE_RDONLY, &mounted) == ERR_NOT_FOUND);
        CHECK(!is_dir(save));
        end();
    }
}

static void test_other_names_and_modes_are_not_found(void) {
    begin("other", "CUSA07001");
    char other[1200], startup[1200];
    save_dir_path(other, sizeof(other), "CUSA07001", "SAVEDATA01");
    save_dir_path(startup, sizeof(startup), "CUSA07001", "SAVEDATA00");
    MountResult mounted;
    CHECK(mount_save(1, "SAVEDATA01", MODE_RDONLY, &mounted) == ERR_NOT_FOUND);
    CHECK(mount_save(1, "SAVEDATA00", 2, &mounted) == ERR_NOT_FOUND);
    CHECK(mount_save(1, "SAVEDATA00", 3, &mounted) == ERR_NOT_FOUND);
    CHECK(!is_dir(other) && !is_dir(startup));
    end();
}

static void test_guest_create_mount_then_existing_load(void) {
    begin("create", "CUSA07001");
    char save[1200], param[1300], save_file[1300], us_save[1200];
    save_dir_path(save, sizeof(save), "CUSA07001", "SAVEDATA00");
    save_dir_path(us_save, sizeof(us_save), "CUSA10454", "SAVEDATA00");
    snprintf(param, sizeof(param), "%s.sce_sys/param.bin", save);
    snprintf(save_file, sizeof(save_file), "%s/SAVE0.DAT", save);

    MountResult created;
    CHECK(mount_save(1, "SAVEDATA00", GUEST_CREATE_MODE, &created) == 0);
    CHECK(created.status == 1);
    CHECK(is_dir(save) && is_file(param) && !is_dir(us_save));
    CHECK(!is_file(save_file));
    CHECK(!strcmp(mounted_host, save));
    write_text(save_file, "guest save");
    unmount_save(&created);

    MountResult loaded;
    CHECK(mount_save(1, "SAVEDATA00", MODE_RDONLY, &loaded) == 0);
    CHECK(loaded.status == 0);
    char text[64];
    CHECK(read_text(save_file, text, sizeof(text)) && !strcmp(text, "guest save"));
    unmount_save(&loaded);
    end();
}

static void test_existing_save_is_not_recreated(void) {
    begin("existing", "CUSA07001");
    char save[1200], meta[1300], save_file[1300], param_file[1300], text[64];
    save_dir_path(save, sizeof(save), "CUSA07001", "SAVEDATA00");
    snprintf(meta, sizeof(meta), "%s.sce_sys", save);
    snprintf(save_file, sizeof(save_file), "%s/SAVE0.DAT", save);
    snprintf(param_file, sizeof(param_file), "%s/param.bin", meta);
    tree_mkdir(save);
    tree_mkdir(meta);
    write_text(save_file, "keep");
    write_text(param_file, "param");

    MountResult mounted;
    CHECK(mount_save(1, "SAVEDATA00", MODE_RDONLY, &mounted) == 0 && mounted.status == 0);
    unmount_save(&mounted);
    CHECK(mount_save(1, "SAVEDATA00", GUEST_CREATE_MODE, &mounted) == ERR_EXISTS);
    CHECK(read_text(save_file, text, sizeof(text)) && !strcmp(text, "keep"));
    CHECK(read_text(param_file, text, sizeof(text)) && !strcmp(text, "param"));
    end();
}

static void test_create_failure_propagates(void) {
    begin("create_failure", "CUSA07001");
    char parent[1200], blocker[1300];
    snprintf(parent, sizeof(parent), "%s/savedata/1", user_dir);
    snprintf(blocker, sizeof(blocker), "%s/CUSA07001", parent);
    tree_mkdir(parent);
    write_text(blocker, "not a directory");

    MountResult mounted;
    CHECK(mount_save(1, "SAVEDATA00", MODE_CREATE, &mounted) == ERR_INTERNAL);
    CHECK(is_file(blocker));
    end();
}

enum { IMAGE_SIZE = 0x11e8060, STUB_ROOM = 0x1000 };
enum { SCENE_BYTES = 0x100, CONTROLLER_BYTES = 0x200, T_CATEGORY = 0x70, T_BLOCK_OWNER = 0xe8 };
enum { STEP_ASK = 1, STEP_CREATE = 3, STEP_TITLE = 4, CATEGORY_NONE = 0, CATEGORY_MISSING = 3, CATEGORY_OTHER = 6 };

static uint64_t block_owner_seen, accepted_block, accepted_controller;
static int accept_calls;
static uint64_t GUEST_HOOK_ABI fake_block(uint64_t owner) {
    block_owner_seen = owner;
    return owner + 0x20eb07;
}
static void GUEST_HOOK_ABI fake_accept(uint64_t block, uint64_t controller) {
    ++accept_calls;
    accepted_block = block;
    accepted_controller = controller;
}

typedef struct {
    uint8_t controller[CONTROLLER_BYTES];
    uint8_t scene[SCENE_BYTES];
} Startup;

static void startup_setup(Startup *s, int32_t category) {
    memset(s, 0, sizeof(*s));
    uint64_t owner = 0x1000;
    memcpy(s->controller + T_BLOCK_OWNER, &owner, sizeof(owner));
    memcpy(s->scene + T_CATEGORY, &category, sizeof(category));
    startup_save_set_functions(fake_block, fake_accept);
    block_owner_seen = accepted_block = accepted_controller = 0;
    accept_calls = 0;
}

static uint64_t set_step(Startup *s, uint64_t kind, uint64_t step) {
    GuestRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.r12 = (uint64_t)(uintptr_t)s->controller;
    regs.r15 = (uint64_t)(uintptr_t)s->scene;
    regs.rsi = kind;
    regs.rdx = step;
    startup_save_step_hook(&regs);
    return regs.rdx;
}

static void set_category(Startup *s, int32_t category) {
    memcpy(s->scene + T_CATEGORY, &category, sizeof(category));
}

static uint8_t *new_image(const StartupSaveRecord *r, int valid) {
    uint8_t *image = calloc(1, IMAGE_SIZE + STUB_ROOM);
    if (!image) return NULL;
    memcpy(image + r->step_offset, r->step_original, STARTUP_SAVE_SITE_LENGTH);
    memcpy(image + r->save_offset, r->save_prologue, STARTUP_SAVE_PROLOGUE_LENGTH);
    memcpy(image + r->accept_offset, r->accept_prologue, STARTUP_SAVE_PROLOGUE_LENGTH);
    if (!valid) image[r->accept_offset] ^= 1;
    return image;
}

static void test_missing_save_enters_the_guest_create_step(void) {
    Startup s;
    startup_setup(&s, CATEGORY_MISSING);
    CHECK(set_step(&s, 1, STEP_ASK) == STEP_CREATE);
    CHECK(accept_calls == 0);
}

static void test_finished_create_continues_through_the_successful_load_path(void) {
    Startup s;
    startup_setup(&s, CATEGORY_MISSING);
    set_step(&s, 1, STEP_ASK);
    set_category(&s, CATEGORY_NONE);
    CHECK(set_step(&s, 1, STEP_TITLE) == STEP_TITLE);
    CHECK(accept_calls == 1);
    CHECK(block_owner_seen == 0x1000 && accepted_block == 0x1000 + 0x20eb07);
    CHECK(accepted_controller == (uint64_t)(uintptr_t)s.controller);
    CHECK(set_step(&s, 1, STEP_TITLE) == STEP_TITLE && accept_calls == 1);
}

static void test_failed_create_does_not_continue(void) {
    Startup s;
    startup_setup(&s, CATEGORY_MISSING);
    set_step(&s, 1, STEP_ASK);
    set_category(&s, CATEGORY_OTHER);
    set_step(&s, 1, STEP_TITLE);
    CHECK(accept_calls == 0);
}

static void test_existing_save_runs_no_compatibility_behavior(void) {
    Startup s;
    startup_setup(&s, CATEGORY_NONE);
    CHECK(set_step(&s, 1, STEP_ASK) == STEP_ASK);
    CHECK(set_step(&s, 1, STEP_TITLE) == STEP_TITLE);
    CHECK(accept_calls == 0);
}

static void test_other_errors_keep_the_question(void) {
    Startup s;
    startup_setup(&s, CATEGORY_OTHER);
    CHECK(set_step(&s, 1, STEP_ASK) == STEP_ASK);
    CHECK(set_step(&s, 2, STEP_ASK) == STEP_ASK);
}

static void test_records_are_explicit_per_build(void) {
    CHECK(!strcmp(startup_save_records[0].build, "CUSA07001 1.00 Japan"));
    CHECK(startup_save_records[0].step_offset == 0x4dbb00 && startup_save_records[0].save_offset == 0x812c0 &&
          startup_save_records[0].accept_offset == 0x8b230 && startup_save_records[0].image.entry == 0x54a800);
    CHECK(!strcmp(startup_save_records[1].build, "CUSA07001 1.02 Japan"));
    CHECK(startup_save_records[1].step_offset == 0x4dc060 && startup_save_records[1].save_offset == 0x813a0 &&
          startup_save_records[1].accept_offset == 0x8b4d0 && startup_save_records[1].image.entry == 0x54a660);
    CHECK(!strcmp(startup_save_records[2].build, "CUSA07001 1.03 Japan"));
    CHECK(startup_save_records[2].step_offset == 0x4dc120 && startup_save_records[2].save_offset == 0x813a0 &&
          startup_save_records[2].accept_offset == 0x8b4d0 && startup_save_records[2].image.entry == 0x54a720);
}

static void test_each_japanese_build_installs_its_own_record(void) {
    for (size_t i = 0; i < STARTUP_SAVE_RECORD_COUNT; ++i) {
        const StartupSaveRecord *r = &startup_save_records[i];
        uint8_t *image = new_image(r, 1);
        CHECK(image != NULL);
        if (!image) return;
        CHECK(startup_save_hook_install(image, IMAGE_SIZE, r->image.entry, image + IMAGE_SIZE, STUB_ROOM) == 1);
        CHECK(image[r->step_offset] == 0xe9);
        free(image);
    }
}

static void test_wrong_original_bytes_reject_installation(void) {
    const StartupSaveRecord *r = &startup_save_records[2];
    uint8_t *image = new_image(r, 1);
    CHECK(image != NULL);
    if (!image) return;
    image[r->step_offset + 1] ^= 1;
    CHECK(startup_save_hook_install(image, IMAGE_SIZE, r->image.entry, image + IMAGE_SIZE, STUB_ROOM) == 0);
    CHECK(image[r->step_offset] == 0x48);
    free(image);
    image = new_image(r, 0);
    CHECK(image != NULL);
    if (!image) return;
    CHECK(startup_save_hook_install(image, IMAGE_SIZE, r->image.entry, image + IMAGE_SIZE, STUB_ROOM) == 0);
    CHECK(image[r->step_offset] == 0x48);
    free(image);
}

static void test_us_and_other_builds_get_no_hook(void) {
    const StartupSaveRecord *r = &startup_save_records[2];
    const struct { uint64_t size, entry; } others[] = {
        {0x11f8060, 0x54c690},
        {IMAGE_SIZE, 0x54c690},
        {IMAGE_SIZE, 0x54a721},
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(*others); ++i) {
        uint8_t *image = calloc(1, others[i].size + STUB_ROOM);
        CHECK(image != NULL);
        if (!image) return;
        memcpy(image + r->step_offset, r->step_original, STARTUP_SAVE_SITE_LENGTH);
        CHECK(startup_save_hook_install(image, others[i].size, others[i].entry, image + others[i].size, STUB_ROOM) == 0);
        CHECK(image[r->step_offset] == 0x48);
        free(image);
    }
}

static void test_a_build_never_uses_another_builds_record(void) {
    const StartupSaveRecord *v103 = &startup_save_records[2];
    uint8_t *image = new_image(v103, 1);
    CHECK(image != NULL);
    if (!image) return;
    CHECK(startup_save_hook_install(image, IMAGE_SIZE, startup_save_records[1].image.entry, image + IMAGE_SIZE, STUB_ROOM) == 0);
    CHECK(image[v103->step_offset] == 0x48);
    free(image);
}

int main(void) {
    test_missing_save_is_not_found_for_every_title();
    test_other_names_and_modes_are_not_found();
    test_guest_create_mount_then_existing_load();
    test_existing_save_is_not_recreated();
    test_create_failure_propagates();
    test_missing_save_enters_the_guest_create_step();
    test_finished_create_continues_through_the_successful_load_path();
    test_failed_create_does_not_continue();
    test_existing_save_runs_no_compatibility_behavior();
    test_other_errors_keep_the_question();
    test_records_are_explicit_per_build();
    test_each_japanese_build_installs_its_own_record();
    test_wrong_original_bytes_reject_installation();
    test_us_and_other_builds_get_no_hook();
    test_a_build_never_uses_another_builds_record();
    printf("savedata tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
