// Focused tests of the launcher's Save page: discovery and overview, DATA slots, backups, export, import, restore, the
// Editor invalidation contract and the page's source routing. Synthetic saves in a temporary folder only; no window, no
// game, no file chooser.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <utility>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "editor_model.h"
#include "game_profile.h"
#include "crypto.h"
#include "options.h"
#include "page_icons.h"
#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"
#include "save_import.h"
#include "save_page_model.h"
#include "save_ps3.h"
#include "save_ps3_layout.h"
#include "save_sfo.h"
#include "save_zip.h"
#include "test_progress.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using Bytes = std::vector<uint8_t>;

void PutU32(Bytes& bytes, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

size_t RecordOffset(int slot, int character) { return slot * kDataSlotStride + kCharacterOffset + character * kCharacterStride; }

void SetCharacter(Bytes& bytes, int slot, int character, const char* name, uint8_t level, uint8_t klass) {
    const size_t base = RecordOffset(slot, character);
    PutU32(bytes, base, 0);
    bytes[base + kCharLevel] = level;
    bytes[base + kCharClass] = klass;
    bytes[base + kCharColor] = 0;
    std::memset(bytes.data() + base + kCharName, 0, kCharNameBytes);
    std::memcpy(bytes.data() + base + kCharName, name, std::strlen(name));
}

void Seal(Bytes& bytes) { PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes)); }

// DATA 1: two characters, DATA 2: empty, DATA 3: one character. `seed` varies the unknown bytes between saves.
Bytes MakeSave(uint8_t seed = 0) {
    Bytes bytes(kSaveFileSize, 0xCC);
    std::memcpy(bytes.data() + kSaveMagicOffset, "DCSD", 4);
    const uint32_t gold[] = {1234567, 7757, 42};
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) PutU32(bytes, slot * kDataSlotStride + kDataSlotGoldOffset, gold[slot]);
    SetCharacter(bytes, 0, 0, "Eppie", 14, 0);
    SetCharacter(bytes, 0, 1, "Allie", 0, 1);
    SetCharacter(bytes, 2, 3, "Morgan", 98, 5);
    for (size_t i = 0x100; i < 0x100 + 64; ++i) bytes[i] = uint8_t(seed + i * 13);
    Seal(bytes);
    return bytes;
}

void WriteBytes(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

Bytes ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Source(const char* relative) { return ReadAll(fs::path(DCP_SOURCE_DIR) / relative); }

// The body of the function that starts at `signature`, up to the next function at column 0.
std::string Body(const std::string& text, const std::string& signature) {
    const size_t begin = text.find(signature);
    if (begin == std::string::npos) return {};
    const size_t end = text.find("\n}\n", begin);
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

fs::path SaveFile(const fs::path& user, const char* title, const char* dir) {
    return user / "savedata" / "1" / title / dir / "SAVE0.DAT";
}

DiscoveredSave Only(const fs::path& user, const char* title = "CUSA10454", const char* dir = "SAVEDATA00") {
    for (DiscoveredSave& save : DiscoverProSaves(user, "CUSA10454")) {
        if (save.title_id == title && save.dir_name == dir) return save;
    }
    CHECK(!"save not discovered");
    return {};
}

size_t CountFiles(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return 0;
    return size_t(std::distance(fs::directory_iterator(folder), fs::directory_iterator()));
}

size_t CountTree(const fs::path& folder) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return 0;
    return size_t(std::distance(fs::recursive_directory_iterator(folder), fs::recursive_directory_iterator()));
}

// ---- Registration, discovery, overview ----

void TestRegistration() {
    int saves = 0;
    for (size_t i = 0; i < std::size(kPageOrder); ++i) {
        if (kPageOrder[i] != PageId::kSave) continue;
        ++saves;
        CHECK(i > 0 && kPageOrder[i - 1] == PageId::kGameplay);
        CHECK(i + 1 < std::size(kPageOrder) && kPageOrder[i + 1] == PageId::kEditor);
    }
    CHECK(saves == 1);
    CHECK(std::string(PageName(PageId::kSave)) == "Saves");
    const PageIconAsset* icon = FindPageIcon(PageId::kSave);
    CHECK(icon && std::string(icon->file) == "save.svg");
    CHECK(RenderPageIcon(PageId::kSave, 24).size() == 24u * 24u * 4u);

    const std::string launcher = Source("launcher/launcher.cpp");
    const size_t page_case = launcher.find("case PageId::kSave:");
    CHECK(page_case != std::string::npos && launcher.find("PageSave(ui)", page_case) < launcher.find("case PageId::", page_case + 1));
    CHECK(launcher.find("save_page_->Draw(ui);") != std::string::npos);
    // The Editor and the Save page share one selection; the game's start is the mutation guard.
    CHECK(launcher.find("&save_selection_);") != std::string::npos);
    CHECK(launcher.find("save_page_ = std::make_unique<SavePage>(UserDir(), save_selection_, SavePageHooks{") != std::string::npos);
    CHECK(launcher.find("[this] { return play_.starting(); },") != std::string::npos);
    CHECK(launcher.find("[this] { return build_; },") != std::string::npos); // the build the Editor gets too
    CHECK(launcher.find("[this] { return editor_page_ && editor_page_->dirty(); },") != std::string::npos);
    // Native choosers where the desktop has them, the launcher's own folder chooser elsewhere; never a new dialog.
    const std::string choose = Body(launcher, "    void ChooseForSavePage(SavePick pick) {");
    CHECK(choose.find("trg::BrowseForFile(") != std::string::npos && choose.find("trg::BrowseForFolder(") != std::string::npos);
    CHECK(choose.find("UseBuiltInChooser()") != std::string::npos && choose.find("InstallSource::kFolder") != std::string::npos);
    CHECK(launcher.find("save_page_->Picked(*browser_save_pick_, *picked);") != std::string::npos);

    // Loading view on the first activation, rediscovery on every entry, one scanner.
    const std::string page = Source("launcher/save_page.cpp");
    const std::string draw = Body(page, "void SavePage::Draw(trg::Ui& ui) {");
    CHECK(draw.find("activation_.Entered(ImGui::GetFrameCount())") != std::string::npos);
    CHECK(draw.find("DrawLoadingView(ui, \"Loading saves...\")") < draw.find("Refresh();"));
    CHECK(Body(page, "void SavePage::Refresh() {").find("DiscoverProSaves(") != std::string::npos);
    const std::string model = Source("launcher/save_page_model.cpp");
    CHECK(page.find("directory_iterator") == std::string::npos && model.find("directory_iterator") == std::string::npos);
    CHECK(model.find("DiscoverSaves(user_dir, active_title_id)") != std::string::npos);
}

void TestDiscoveryAndOverview(const fs::path& base) {
    const fs::path user = base / "discover";
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA00"), MakeSave());
    Bytes corrupt = MakeSave(7);
    corrupt[0x200] ^= 0x55; // CRC no longer matches
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA01"), corrupt);
    WriteBytes(SaveFile(user, "CUSA07001", "SAVEDATA00"), MakeSave(3));
    WriteBytes(SaveFile(user, "PCSE00019", "SAVEDATA00"), MakeSave(4)); // not a Pro title: not listed

    const std::vector<DiscoveredSave> saves = DiscoverProSaves(user, "CUSA10454");
    CHECK(saves.size() == 3);
    for (const DiscoveredSave& save : saves) CHECK(save.title_id != "PCSE00019");
    const std::vector<size_t> visible = SavesOfTitle(saves, "CUSA10454");
    const int chosen = ChooseSave(saves, visible, {});
    CHECK(visible.size() == 2 && chosen >= 0 && saves[chosen].title_id == "CUSA10454" && saves[chosen].dir_name == "SAVEDATA00" &&
          saves[chosen].current);
    const fs::path other_title[] = {SaveFile(user, "CUSA07001", "SAVEDATA00")};
    CHECK(ChooseSave(saves, visible, other_title) == chosen); // a save of another title is never chosen
    const fs::path second[] = {SaveFile(user, "CUSA10454", "SAVEDATA01")};
    CHECK(ChooseSave(saves, visible, second) >= 0 && saves[ChooseSave(saves, visible, second)].dir_name == "SAVEDATA01");
    const fs::path missing[] = {SaveFile(user, "CUSA10454", "MISSING")};
    CHECK(ChooseSave(saves, visible, missing) == chosen);
    CHECK(ChooseSave({}, {}, {}) == -1);

    const DiscoveredSave& valid = saves[chosen];
    const SaveOverview overview = DescribeOverview(valid);
    CHECK(overview.platform == "PS4" && overview.region == "USA" && overview.title_id == "CUSA10454");
    CHECK(overview.save_name == "SAVEDATA00" && overview.current);
    CHECK(overview.status == SaveFileStatus::kValid && std::string(SaveStatusText(overview.status)) == "Valid");
    CHECK(std::string(CrcStatusText(overview.status)) == "Valid");
    CHECK(overview.populated_slots == 2 && overview.characters == 3);
    CHECK(overview.size == "2.1 MB" && overview.modified.size() == 16);
    CHECK(SaveIdentity(valid) == "CUSA10454 / SAVEDATA00 (USA)");
    const std::vector<size_t> japan = SavesOfTitle(saves, "CUSA07001");
    CHECK(japan.size() == 1 && DescribeOverview(saves[japan[0]]).region == "Japan");

    const DiscoveredSave* invalid = nullptr;
    for (const DiscoveredSave& save : saves) {
        if (save.dir_name == "SAVEDATA01") invalid = &save;
    }
    CHECK(invalid && !invalid->error.empty());
    const SaveOverview bad = DescribeOverview(*invalid);
    CHECK(bad.status == SaveFileStatus::kBadCrc && std::string(CrcStatusText(bad.status)) == "Invalid");
    CHECK(bad.populated_slots == 0 && bad.characters == 0);

    CHECK(ClassifySave(SaveModel::FromBytes(Bytes(100, 0))) == SaveFileStatus::kWrongSize);
    Bytes wrong_magic = MakeSave();
    std::memcpy(wrong_magic.data() + kSaveMagicOffset, "DSCD", 4); // a byte-swapped magic is not accepted
    Seal(wrong_magic);
    CHECK(ClassifySave(SaveModel::FromBytes(wrong_magic)) == SaveFileStatus::kNotDragonsCrown);
    CHECK(std::string(CrcStatusText(SaveFileStatus::kNotDragonsCrown)) == "Not checked");
    CHECK(ClassifySave(SaveModel::FromFile(user / "missing" / "SAVE0.DAT")) == SaveFileStatus::kUnreadable);
}

void WriteEveryTitle(const fs::path& user) {
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA00"), MakeSave(1));
    WriteBytes(SaveFile(user, "CUSA07001", "SAVEDATA00"), MakeSave(2));
    WriteBytes(SaveFile(user, "CUSA10209", "SAVEDATA00"), MakeSave(3));
    WriteBytes(SaveFile(user, "CUSA10487", "SAVEDATA00"), MakeSave(4));
}

const GameBuild* BuildOfTitle(std::string_view title_id) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (title_id == build.title_id) return &build;
    }
    CHECK(!"unknown title id");
    return nullptr;
}

// Only the saves of the installed game's title are ever visible, and the Editor reads the same set.
void TestInstalledGameScope(const fs::path& base) {
    const fs::path user = base / "scope";
    WriteEveryTitle(user);
    const std::vector<DiscoveredSave> saves = DiscoverProSaves(user, "CUSA10454");
    CHECK(saves.size() == 4);
    for (const char* installed : {"CUSA10454", "CUSA07001", "CUSA10209", "CUSA10487"}) {
        const std::vector<size_t> visible = SavesOfTitle(saves, installed);
        CHECK(visible.size() == 1 && saves[visible[0]].title_id == installed);
        for (const size_t index : visible) CHECK(saves[index].title_id == installed);
        const int chosen = ChooseSave(saves, visible, {});
        CHECK(chosen == int(visible[0]));

        const GameBuild* build = BuildOfTitle(installed);
        EditorBuild editor;
        editor.build = build;
        CHECK(SavesOfBuild(saves, editor) == visible);
    }
    CHECK(SavesOfTitle(saves, "").empty() && SavesOfTitle(saves, "CUSA99999").empty());
    EditorBuild none;
    CHECK(SavesOfBuild(saves, none).empty());

    // Another title's saved selection never survives into the installed game's list.
    const std::vector<size_t> installed = SavesOfTitle(saves, "CUSA10454");
    const fs::path stale_selection[] = {SaveFile(user, "CUSA10209", "SAVEDATA00")};
    CHECK(saves[ChooseSave(saves, installed, stale_selection)].title_id == "CUSA10454");
    CHECK(ChooseSave(saves, SavesOfTitle(saves, "CUSA99999"), stale_selection) == -1);

    // Several save folders of the same title stay usable: all visible, the preferred one wins, a broken one is not the default.
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA01"), MakeSave(5));
    Bytes broken = MakeSave(6);
    broken[0] ^= 1;
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA02"), broken);
    const std::vector<DiscoveredSave> several = DiscoverProSaves(user, "CUSA10454");
    const std::vector<size_t> own = SavesOfTitle(several, "CUSA10454");
    CHECK(own.size() == 3);
    for (const size_t index : own) CHECK(several[index].title_id == "CUSA10454");
    CHECK(several[ChooseSave(several, own, {})].dir_name == "SAVEDATA00");
    const fs::path pick[] = {SaveFile(user, "CUSA10454", "SAVEDATA01")};
    CHECK(several[ChooseSave(several, own, pick)].dir_name == "SAVEDATA01");
    const fs::path pick_broken[] = {SaveFile(user, "CUSA10454", "SAVEDATA02")};
    const DiscoveredSave& listed_invalid = several[ChooseSave(several, own, pick_broken)];
    CHECK(listed_invalid.dir_name == "SAVEDATA02" &&
          !ActionStateFor(SaveAction::kRestore, &listed_invalid, {}).enabled);

    // The page and the Editor use the same helpers, and the page offers no way to leave the installed title.
    const std::string page = Source("launcher/save_page.cpp");
    const std::string editor = Source("launcher/editor_page.cpp");
    CHECK(Body(page, "void SavePage::Refresh() {").find("SavesOfTitle(saves_, installed_title_)") != std::string::npos);
    CHECK(Body(page, "void SavePage::Refresh() {").find("ChooseSave(saves_, visible_, preferred)") != std::string::npos);
    CHECK(Body(editor, "void EditorPage::Refresh() {").find("ChooseSave(saves_, visible, preferred)") != std::string::npos);
    CHECK(Body(editor, "std::vector<size_t> EditorPage::VisibleSaves() const {").find("SavesOfBuild(saves_, *build)") != std::string::npos);
    CHECK(Source("launcher/editor_model.cpp").find("SavesOfTitle(saves, build.build->title_id)") != std::string::npos);
    CHECK(page.find("title_id ==") == std::string::npos && Source("launcher/save_page_model.cpp").find("DiscoverSaves(user_dir") != std::string::npos);
    const std::string selector = Body(page, "void SavePage::DrawSelector(trg::Ui& ui) {");
    CHECK(selector.find("if (visible_.size() < 2) return;") != std::string::npos);
    CHECK(selector.find("for (const size_t index : visible_)") != std::string::npos && selector.find("saves_.size()") == std::string::npos);
    CHECK(selector.find("title_id") == std::string::npos && selector.find("SaveIdentity") == std::string::npos);
    size_t selects = 0; // its definition and the two callers that stay inside the visible set
    for (size_t at = page.find("Select("); at != std::string::npos; at = page.find("Select(", at + 1)) ++selects;
    CHECK(selects == 3);
    CHECK(Body(page, "void SavePage::Draw(trg::Ui& ui) {").find("installed_title_.empty()") != std::string::npos);
    CHECK(Source("launcher/save_page.h").find("installed_build") != std::string::npos &&
          Source("launcher/save_page.h").find("active_title_id") == std::string::npos);
}

// The destination of an import and of a restore is the installed title's save, whatever the source was.
void TestInstalledDestination(const fs::path& base) {
    const fs::path user = base / "destination";
    WriteEveryTitle(user);
    const fs::path folder = BackupFolder(user);
    const std::vector<DiscoveredSave> saves = DiscoverProSaves(user, "CUSA10454");
    const DiscoveredSave destination = saves[SavesOfTitle(saves, "CUSA10454")[0]];
    const fs::path destination_file = destination.save0_path;
    const Bytes foreign = ReadBytes(SaveFile(user, "CUSA10209", "SAVEDATA00"));
    const Bytes japanese = ReadBytes(SaveFile(user, "CUSA07001", "SAVEDATA00"));

    // A source from another build is accepted as a source and written only over the installed game's save.
    const ImportPlan plan = PlanImport(SaveFile(user, "CUSA10209", "SAVEDATA00"), destination, ImportMode::kPs4);
    CHECK(plan.refused.empty() && plan.inspected.title_id == "CUSA10209" && plan.destination == destination_file);
    CHECK(plan.destination_identity == "CUSA10454 / SAVEDATA00 (USA)");
    const SaveOpResult imported = CommitImportPlan(plan, destination, folder);
    CHECK(imported.ok && ReadBytes(destination_file) == foreign);
    CHECK(ReadBytes(SaveFile(user, "CUSA07001", "SAVEDATA00")) == japanese && ReadBytes(SaveFile(user, "CUSA10209", "SAVEDATA00")) == foreign);
    CHECK(ReadBytes(SaveFile(user, "CUSA10487", "SAVEDATA00")) == MakeSave(4));

    // The plan cannot be committed over any other save.
    const DiscoveredSave other = saves[SavesOfTitle(saves, "CUSA07001")[0]];
    CHECK(!CommitImportPlan(plan, other, folder).ok && ReadBytes(other.save0_path) == japanese);

    // Backups of other titles are not offered for this save; a backup is listed only for its own title and folder.
    CHECK(BackupSave(other, folder).ok && BackupSave(Only(user), folder).ok);
    const std::vector<BackupInfo> all = ListBackups(folder);
    const DiscoveredSave now = Only(user);
    for (const BackupRow& row : BackupsOfSave(all, now)) CHECK(row.info.title_id == "CUSA10454" && row.info.dir_name == "SAVEDATA00");
    CHECK(!BackupsOfSave(all, now).empty() && all.size() > BackupsOfSave(all, now).size());
    CHECK(BackupsOfSave(all, other).size() == 1 && BackupsOfSave(all, other)[0].info.title_id == "CUSA07001");

    // Restoring writes only the destination it was given.
    const BackupRow own = BackupsOfSave(all, now).front();
    const Bytes before_other = ReadBytes(other.save0_path);
    CHECK(RestoreToSave(own.info, now, folder).ok && ReadBytes(other.save0_path) == before_other);

    // The page asks for the destination only through Selected().
    const std::string page = Source("launcher/save_page.cpp");
    CHECK(Body(page, "void SavePage::Picked(").find("PlanImport(path, *save, ps3 ? ImportMode::kPs3 : ImportMode::kPs4)") != std::string::npos);
    CHECK(Body(page, "void SavePage::Picked(").find("const DiscoveredSave* save = Selected();") != std::string::npos);
    CHECK(Body(page, "void SavePage::DrawBackupConfirm(").find("RestoreToSave(done.info, save,") != std::string::npos);
}

void TestSlots() {
    const std::vector<SlotOverview> slots = DescribeSlots(SaveModel::FromBytes(MakeSave()));
    CHECK(slots.size() == kDataSlotCount);
    if (slots.size() != kDataSlotCount) return;
    CHECK(slots[0].name == "DATA 1" && slots[0].gold == 1234567 && slots[0].characters.size() == 2);
    CHECK(slots[0].characters[0].name == "Eppie" && slots[0].characters[0].level == 15 &&
          slots[0].characters[0].klass == CharacterClass::kFighter);
    CHECK(slots[0].characters[1].klass == CharacterClass::kAmazon && slots[0].characters[1].level == 1);
    CHECK(slots[1].name == "DATA 2" && slots[1].empty() && slots[1].gold == 7757);
    CHECK(slots[2].characters.size() == 1 && slots[2].characters[0].name == "Morgan" && slots[2].characters[0].level == 99 &&
          slots[2].characters[0].klass == CharacterClass::kWizard);
    Bytes bad = MakeSave();
    bad[0] ^= 1;
    CHECK(DescribeSlots(SaveModel::FromBytes(bad)).empty());
}

void TestGuards() {
    DiscoveredSave valid;
    valid.model = SaveModel::FromBytes(MakeSave());
    Bytes corrupt = MakeSave();
    corrupt[0] ^= 1;
    DiscoveredSave invalid;
    invalid.model = SaveModel::FromBytes(corrupt);
    for (const SaveAction action : {SaveAction::kBackup, SaveAction::kExport, SaveAction::kImport, SaveAction::kRestore}) {
        CHECK(ActionStateFor(action, &valid, {}).enabled);
        CHECK(!ActionStateFor(action, nullptr, {}).enabled);
        CHECK(!ActionStateFor(action, &invalid, {}).enabled && *ActionStateFor(action, &invalid, {}).reason);
        CHECK(!ActionStateFor(action, &valid, {.game_active = true}).enabled);
    }
    // Pending Editor changes hold back only what replaces the save.
    const SaveGuards dirty{.editor_dirty = true};
    CHECK(ActionStateFor(SaveAction::kBackup, &valid, dirty).enabled && ActionStateFor(SaveAction::kExport, &valid, dirty).enabled);
    CHECK(!ActionStateFor(SaveAction::kImport, &valid, dirty).enabled && !ActionStateFor(SaveAction::kRestore, &valid, dirty).enabled);
    CHECK(std::string(ActionStateFor(SaveAction::kRestore, &valid, dirty).reason).find("Editor") != std::string::npos);

    // The page asks again at the moment of writing, not only when it drew the button.
    const std::string page = Source("launcher/save_page.cpp");
    CHECK(Body(page, "void SavePage::DrawBackupConfirm(").find("if (!ActionStateFor(SaveAction::kRestore, &save, Guards()).enabled) return;") != std::string::npos);
    CHECK(page.find("if (confirmed && ActionStateFor(SaveAction::kImport, save, Guards()).enabled) {") != std::string::npos);
    CHECK(page.find("if (!ActionStateFor(SaveAction::kImport, save, Guards()).enabled) return;") != std::string::npos);
    CHECK(page.find("if (!ActionStateFor(SaveAction::kExport, save, Guards()).enabled) return;") != std::string::npos);
    // Every replacement goes through the launcher's confirmation.
    CHECK(Body(page, "void SavePage::DrawBackupConfirm(").find("ui.AccentButton(") < Body(page, "void SavePage::DrawBackupConfirm(").find("RestoreToSave("));
    CHECK(Body(page, "void SavePage::DrawImportConfirm(").find("ui.ConfirmRow(") <
          Body(page, "void SavePage::DrawImportConfirm(").find("CommitImportPlan("));
    CHECK(Body(page, "void SavePage::Picked(").find("CommitImportPlan(") == std::string::npos);
    CHECK(Body(page, "void SavePage::Select(int index) {").find("CreateBackup(") == std::string::npos && Body(page, "void SavePage::Select(int index) {").find("BackupSave(") == std::string::npos);
}

// ---- Backups ----

void TestBackups(const fs::path& base) {
    const fs::path user = base / "backups";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes original = MakeSave();
    WriteBytes(file, original);
    const auto modified = fs::last_write_time(file);
    const DiscoveredSave save = Only(user);
    const fs::path folder = BackupFolder(user);

    std::set<fs::path> names;
    for (int i = 0; i < 6; ++i) {
        const SaveOpResult result = BackupSave(save, folder);
        CHECK(result.ok && result.backup && ReadBytes(result.backup->file) == original);
        if (result.backup) {
            CHECK(result.backup->file.parent_path() == folder);
            names.insert(result.backup->file);
        }
    }
    CHECK(names.size() == 6); // same millisecond or not, never the same file
    CHECK(ReadBytes(file) == original && fs::last_write_time(file) == modified);

    const std::vector<BackupInfo> listed = ListBackups(folder);
    CHECK(listed.size() == 6);
    for (size_t i = 1; i < listed.size(); ++i) CHECK(listed[i - 1].timestamp >= listed[i].timestamp);
    const std::vector<BackupRow> rows = BackupsOfSave(listed, save);
    CHECK(rows.size() == 6 && rows.front().size == "2.1 MB" && rows.front().when.size() == 19);
    CHECK(BackupTimeText("20261122-101010-123") == "2026-11-22 10:10:10");
    CHECK(BackupTimeText("20261122-101010-123_2") == "2026-11-22 10:10:10");
    CHECK(BackupTimeText("garbage") == "garbage");
    for (const BackupRow& row : rows) CHECK(row.info.file.extension() == ".DAT");
    std::set<std::string> labels;
    for (const BackupRow& row : rows) labels.insert(row.when);
    CHECK(labels.size() == rows.size()); // each row names itself uniquely, whatever the clock resolution
    DiscoveredSave other = save;
    other.dir_name = "SAVEDATA01";
    CHECK(BackupsOfSave(listed, other).empty());

    // An invalid save is never backed up from the page.
    const fs::path bad_user = base / "backups_bad";
    Bytes corrupt = MakeSave();
    corrupt[0] ^= 1;
    WriteBytes(SaveFile(bad_user, "CUSA10454", "SAVEDATA00"), corrupt);
    const SaveOpResult refused = BackupSave(Only(bad_user), BackupFolder(bad_user));
    CHECK(!refused.ok && CountFiles(BackupFolder(bad_user)) == 0);
}

// ---- Backup list: the newest 15 are shown, all stay on disk, one can be deleted safely ----

// Writes a backup the way CreateBackup does, with a chosen timestamp so the order is known.
BackupInfo WriteBackupFiles(const fs::path& folder, int index, const char* title, const char* dir, const Bytes& bytes) {
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "2026%02d%02d-120000-000", index / 28 + 1, index % 28 + 1);
    BackupInfo info;
    info.timestamp = stamp;
    info.title_id = title;
    info.dir_name = dir;
    const std::string base = std::string("SAVE0_") + stamp + "_" + title + "_" + dir;
    info.file = folder / (base + ".DAT");
    info.meta = folder / (base + ".meta");
    WriteBytes(info.file, bytes);
    const std::string meta = std::string("timestamp=") + stamp + "\ntitle_id=" + title + "\ndata_slot=" + dir + "\noriginal_path=x\n";
    WriteBytes(info.meta, Bytes(meta.begin(), meta.end()));
    return info;
}

size_t CountWithExtension(const fs::path& folder, const char* extension) {
    size_t count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) count += entry.path().extension() == extension;
    return count;
}

void TestBackupListing(const fs::path& base) {
    CHECK(kBackupDisplayLimit == 15);
    for (const int total : {0, 1, 15, 16, 130}) {
        const fs::path user = base / ("listing_" + std::to_string(total));
        WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA00"), MakeSave());
        const fs::path folder = BackupFolder(user);
        fs::create_directories(folder);
        for (int i = 0; i < total; ++i) WriteBackupFiles(folder, i, "CUSA10454", "SAVEDATA00", MakeSave(uint8_t(i)));
        WriteBackupFiles(folder, 200, "CUSA10454", "SAVEDATA01", MakeSave(9));
        WriteBackupFiles(folder, 201, "CUSA07001", "SAVEDATA00", MakeSave(9));

        const DiscoveredSave save = Only(user);
        const std::vector<BackupInfo> all = ListBackups(folder);
        const BackupListing listing = LatestBackups(all, save);
        const size_t shown = size_t(std::min(total, 15));
        CHECK(all.size() == size_t(total) + 2);
        CHECK(listing.total == size_t(total) && listing.shown.size() == shown);
        CHECK(BackupsOfSave(all, save).size() == size_t(total));
        for (size_t i = 0; i < listing.shown.size(); ++i) {
            char stamp[32];
            const int newest = total - 1 - int(i);
            std::snprintf(stamp, sizeof(stamp), "2026%02d%02d-120000-000", newest / 28 + 1, newest % 28 + 1);
            CHECK(listing.shown[i].info.timestamp == stamp);
            CHECK(listing.shown[i].info.title_id == "CUSA10454" && listing.shown[i].info.dir_name == "SAVEDATA00");
            CHECK(listing.shown[i].size == "2.1 MB");
        }
        CHECK(CountWithExtension(folder, ".DAT") == size_t(total) + 2 && CountWithExtension(folder, ".meta") == size_t(total) + 2);
    }

    // Creating a backup never removes one: the 16th is listed first and the oldest visible one only leaves the page.
    const fs::path user = base / "listing_create";
    WriteBytes(SaveFile(user, "CUSA10454", "SAVEDATA00"), MakeSave());
    const fs::path folder = BackupFolder(user);
    fs::create_directories(folder);
    for (int i = 0; i < 15; ++i) WriteBackupFiles(folder, i, "CUSA10454", "SAVEDATA00", MakeSave());
    const DiscoveredSave save = Only(user);
    const std::string oldest = LatestBackups(ListBackups(folder), save).shown.back().info.timestamp;
    CHECK(BackupSave(save, folder).ok);
    const BackupListing after = LatestBackups(ListBackups(folder), save);
    CHECK(after.total == 16 && after.shown.size() == 15 && after.shown.front().info.timestamp > after.shown[1].info.timestamp);
    CHECK(after.shown.back().info.timestamp != oldest && CountWithExtension(folder, ".DAT") == 16);
}

void TestBackupDelete(const fs::path& base) {
    const fs::path user = base / "delete";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave(3));
    const fs::path folder = BackupFolder(user);
    fs::create_directories(folder);
    for (int i = 0; i < 20; ++i) WriteBackupFiles(folder, i, "CUSA10454", "SAVEDATA00", MakeSave(uint8_t(i)));
    const BackupInfo neighbour = WriteBackupFiles(folder, 100, "CUSA10454", "SAVEDATA01", MakeSave(1));
    WriteBytes(folder / "SAVE0_lonely.meta", Bytes{'x'});
    WriteBytes(folder / "notes.txt", Bytes{'n'});
    const DiscoveredSave save = Only(user);

    BackupListing listing = LatestBackups(ListBackups(folder), save);
    CHECK(listing.total == 20 && listing.shown.size() == 15);
    const BackupInfo target = listing.shown[4].info;
    const Bytes target_bytes = ReadBytes(target.file);
    const Bytes neighbour_bytes = ReadBytes(neighbour.file);
    CHECK(DeleteSaveBackup(target, save, folder).ok);
    CHECK(!fs::exists(target.file) && !fs::exists(target.meta));
    CHECK(fs::exists(neighbour.file) && fs::exists(neighbour.meta) && ReadBytes(neighbour.file) == neighbour_bytes);
    CHECK(fs::exists(folder / "SAVE0_lonely.meta") && fs::exists(folder / "notes.txt") && ReadBytes(file) == MakeSave(3));
    listing = LatestBackups(ListBackups(folder), save);
    CHECK(listing.total == 19 && listing.shown.size() == 15);
    for (const BackupRow& row : listing.shown) CHECK(row.info.file != target.file);
    for (size_t i = 1; i < listing.shown.size(); ++i) CHECK(listing.shown[i - 1].info.timestamp > listing.shown[i].info.timestamp);

    // A confirmation made on a row that is already gone cannot delete whatever took its place.
    const SaveOpResult again = DeleteSaveBackup(target, save, folder);
    CHECK(!again.ok && !again.error.empty() && LatestBackups(ListBackups(folder), save).total == 19);
    CHECK(!target_bytes.empty() && ReadBytes(listing.shown[4].info.file) != target_bytes);

    // Only backups of the selected save, inside the backup folder, are deleted.
    CHECK(!DeleteSaveBackup(neighbour, save, folder).ok && fs::exists(neighbour.file) && fs::exists(neighbour.meta));
    const fs::path outside = base / "delete_outside";
    fs::create_directories(outside);
    const BackupInfo foreign = WriteBackupFiles(outside, 3, "CUSA10454", "SAVEDATA00", MakeSave(5));
    CHECK(!DeleteSaveBackup(foreign, save, folder).ok && fs::exists(foreign.file) && fs::exists(foreign.meta));
    BackupInfo traversal = listing.shown[0].info;
    const BackupInfo escaped = WriteBackupFiles(outside, 4, "CUSA10454", "SAVEDATA00", MakeSave(6));
    traversal.file = folder / ".." / ".." / "delete_outside" / escaped.file.filename();
    traversal.meta = folder / ".." / ".." / "delete_outside" / escaped.meta.filename();
    CHECK(!DeleteSaveBackup(traversal, save, folder).ok && fs::exists(escaped.file) && fs::exists(escaped.meta));
    BackupInfo hijack = listing.shown[0].info;
    hijack.file = file;
    hijack.meta = file;
    CHECK(!DeleteSaveBackup(hijack, save, folder).ok && ReadBytes(file) == MakeSave(3));
    BackupInfo unmatched = listing.shown[0].info;
    unmatched.file = folder / "notes.DAT";
    unmatched.meta = folder / "notes.meta";
    WriteBytes(unmatched.file, Bytes{'d'});
    CHECK(!DeleteSaveBackup(unmatched, save, folder).ok && fs::exists(unmatched.file));
    CHECK(!DeleteSaveBackup(BackupInfo{}, save, folder).ok && !DeleteSaveBackup(listing.shown[0].info, save, folder / "nowhere").ok);
    CHECK(LatestBackups(ListBackups(folder), save).total == 19);

    // The page uses the backend only, and shows the newest 15 inside one scroll region.
    const std::string page = Source("launcher/save_page.cpp");
    const std::string backups = Body(page, "void SavePage::DrawBackups(");
    CHECK(backups.find("ImGui::BeginChild(\"##backup_list\"") != std::string::npos && backups.find("ImGui::EndChild();") != std::string::npos);
    CHECK(backups.find("listing_.shown") != std::string::npos && backups.find("Showing latest {shown} of {total}") != std::string::npos);
    CHECK(backups.find("title_id") == std::string::npos && backups.find("dir_name") == std::string::npos && backups.find("SaveIdentity") == std::string::npos);
    CHECK(backups.find("ui.scale()") != std::string::npos && backups.find("ImGuiWindowFlags_HorizontalScrollbar") == std::string::npos);
    const std::string confirm = Body(page, "void SavePage::DrawBackupConfirm(");
    CHECK(confirm.find("DeleteSaveBackup(done.info, save,") != std::string::npos && confirm.find("This permanently deletes this backup") != std::string::npos);
    CHECK(confirm.find("remove(") == std::string::npos && page.find("std::filesystem::remove") == std::string::npos);
    CHECK(Body(page, "BackupClick DrawBackupRow(").find("\"Restore...\"") != std::string::npos && Body(page, "BackupClick DrawBackupRow(").find("\"Delete...\"") != std::string::npos);
    CHECK(Body(page, "void SavePage::DrawManagement(").find("\"Open backup folder\"") != std::string::npos);

    // Details is a plain section; the backup list keeps a gutter by the Bug Report page's own helper.
    const std::string details = Body(page, "void SavePage::DrawDetails(");
    CHECK(details.find("ui.Heading(\"Details\");") != std::string::npos && details.find("Section(") == std::string::npos &&
          details.find("CollapsingHeader") == std::string::npos && details.find("if (") == std::string::npos);
    CHECK(details.find("ui.Info(\"File\"") != std::string::npos && details.find("ui.Info(\"Backup folder\"") != std::string::npos);
    CHECK(page.find("CollapsingHeader") == std::string::npos && page.find("TreeNode") == std::string::npos);
    CHECK(backups.find("ImGuiWindowFlags_AlwaysVerticalScrollbar") != std::string::npos && backups.find("HorizontalScrollbar") == std::string::npos &&
          backups.find("NoScrollbar") == std::string::npos && backups.find("kBackupDisplayLimit") == std::string::npos);
    const size_t child = backups.find("ImGui::BeginChild(\"##backup_list\"");
    CHECK(backups.find("if (!scrolls) ReserveScrollbarWidth();", child) != std::string::npos);
    CHECK(backups.find("KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));", child) > backups.find("ReserveScrollbarWidth();", child));
    CHECK(Source("launcher/incident_page.cpp").find("KeepClearOfScrollbar(std::floor(trg::layout::kContentPadding * ui.scale()));") != std::string::npos);
    CHECK(Source("launcher/page_margin.h").find("inline void KeepClearOfScrollbar(float margin)") != std::string::npos);
    CHECK(backups.find("const float outer_gutter = std::floor(trg::layout::kContentPadding * ui.scale());") != std::string::npos &&
          backups.find("ImVec2(-outer_gutter, height)") != std::string::npos && backups.find("ImVec2(0.0f, height)") == std::string::npos);
    CHECK(backups.find("ImGui::EndChild();") > backups.find("DrawBackupRow(row,") && backups.find("SetCursorScreenPos") == std::string::npos);
    const size_t divider_at = backups.find("if (i > 0) DrawBackupDivider(divider, float(kDividerPixels));");
    CHECK(divider_at > backups.find("ImGui::BeginChild(\"##backup_list\"") && divider_at < backups.find("DrawBackupRow(row,") &&
          divider_at < backups.find("ImGui::EndChild();"));
    CHECK(backups.find("trg::Col(ui.theme().border_row)") != std::string::npos);
    const std::string divider_body = Body(page, "void DrawBackupDivider(");
    CHECK(divider_body.find("AddRectFilled(") != std::string::npos && divider_body.find("GetContentRegionAvail().x") != std::string::npos &&
          divider_body.find("Dummy") == std::string::npos && divider_body.find("Separator(") == std::string::npos);
    CHECK(backups.find("const float button = 104.0f * ui.scale();") != std::string::npos && backups.find("const float spacing = 8.0f * ui.scale();") != std::string::npos);
    CHECK(Body(page, "BackupClick DrawBackupRow(").find("start + width - button - button - spacing") != std::string::npos);
    CHECK(Body(page, "void SavePage::DrawBackupConfirm(").find("pending_backup_") != std::string::npos && Source("launcher/save_page.h").find("std::optional<PendingBackup> pending_backup_;") != std::string::npos);
}

void TestBackupMetadataSafety(const fs::path& base) {
    const fs::path user = base / "meta";
    const fs::path folder = BackupFolder(user);
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave());
    const DiscoveredSave save = Only(user);

    // A crafted .meta names folders outside the backup folder.
    const Bytes backup_bytes = MakeSave(9);
    WriteBytes(folder / "SAVE0_crafted.DAT", backup_bytes);
    {
        std::ofstream meta(folder / "SAVE0_crafted.meta", std::ios::binary);
        meta << "timestamp=20990101-000000-000\r\ntitle_id=../../../escape\r\ndata_slot=..\\..\\x\r\noriginal_path=/etc/passwd\r\n";
    }
    // Oversized, and without its .DAT: both skipped.
    {
        std::ofstream meta(folder / "SAVE0_huge.meta", std::ios::binary);
        meta << "title_id=CUSA10454\n" << std::string(8000, 'x') << "\n";
        WriteBytes(folder / "SAVE0_huge.DAT", backup_bytes);
        std::ofstream lonely(folder / "SAVE0_lonely.meta", std::ios::binary);
        lonely << "title_id=CUSA10454\ndata_slot=SAVEDATA00\n";
    }
    const std::vector<BackupInfo> listed = ListBackups(folder);
    CHECK(listed.size() == 1);
    if (listed.size() != 1) return;
    CHECK(listed[0].title_id.empty() && listed[0].dir_name.empty() && listed[0].file == folder / "SAVE0_crafted.DAT");
    CHECK(BackupsOfSave(listed, save).empty()); // it belongs to no save

    // Restoring it still backs the destination up under the destination's own names, inside the backup folder.
    const size_t before = CountTree(base);
    const SaveOpResult restored = RestoreToSave(listed[0], save, folder);
    CHECK(restored.ok && ReadBytes(file) == backup_bytes);
    CHECK(CountTree(base) == before + 2); // one .DAT and one .meta
    for (const fs::directory_entry& entry : fs::directory_iterator(folder)) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find("..") == std::string::npos && name.rfind("SAVE0_", 0) == 0);
    }
    CHECK(!fs::exists(base / "escape") && !fs::exists(user / "escape"));

    // CreateBackup itself reduces names to plain tokens.
    const std::optional<BackupInfo> odd = CreateBackup(folder, file, "../up", "a/b\\c");
    CHECK(odd && odd->file.parent_path() == folder && odd->title_id == "___up" && odd->dir_name == "a_b_c");
}

// ---- Export ----

void TestExport(const fs::path& base) {
    const fs::path user = base / "export";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes original = MakeSave(5);
    WriteBytes(file, original);
    const auto modified = fs::last_write_time(file);
    const DiscoveredSave save = Only(user);
    const fs::path out = base / "export_out";
    fs::create_directories(out);

    ExportPlan plan = PlanExport(save, out, user);
    CHECK(plan.refused.empty() && !plan.exists && plan.destination == out / "SAVE0.DAT");
    SaveOpResult result = ExportSave(save, plan);
    CHECK(result.ok);
    const Bytes exported = ReadBytes(out / "SAVE0.DAT");
    std::vector<std::string> problems;
    CHECK(exported == original && ValidateSave(exported, problems));
    CHECK(ReadBytes(file) == original && fs::last_write_time(file) == modified);
    CHECK(!fs::exists(out / "SAVE0.DAT.tmp") && CountFiles(BackupFolder(user)) == 0);

    // A file already there needs the player's confirmation, then is replaced by an identical copy.
    WriteBytes(out / "SAVE0.DAT", Bytes(10, 1));
    plan = PlanExport(save, out, user);
    CHECK(plan.refused.empty() && plan.exists);
    CHECK(ExportSave(save, plan).ok && ReadBytes(out / "SAVE0.DAT") == original);
    CHECK(Source("launcher/save_page.cpp").find("pending_export_ = std::move(plan);") != std::string::npos);

    CHECK(!PlanExport(save, file.parent_path(), user).refused.empty());                    // the save's own folder
    CHECK(!PlanExport(save, user / "savedata" / "1" / "CUSA07001", user).refused.empty()); // inside savedata
    fs::create_directories(user / "savedata" / "1" / "CUSA07001");
    CHECK(!PlanExport(save, user / "savedata" / "1" / "CUSA07001", user).refused.empty());
    CHECK(!PlanExport(save, base / "missing_folder", user).refused.empty());
    CHECK(!ExportSave(save, PlanExport(save, base / "missing_folder", user)).ok);
    CHECK(!fs::exists(base / "missing_folder"));

    const fs::path bad_user = base / "export_bad";
    Bytes corrupt = original;
    corrupt[0] ^= 1;
    WriteBytes(SaveFile(bad_user, "CUSA10454", "SAVEDATA00"), corrupt);
    const fs::path bad_out = base / "export_bad_out";
    fs::create_directories(bad_out);
    const DiscoveredSave bad = Only(bad_user);
    CHECK(!ExportSave(bad, PlanExport(bad, bad_out, bad_user)).ok && !fs::exists(bad_out / "SAVE0.DAT"));
}

// ---- Import ----

void TestImport(const fs::path& base) {
    const fs::path user = base / "import";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes destination_bytes = MakeSave(1);
    WriteBytes(file, destination_bytes);
    const DiscoveredSave save = Only(user);
    const fs::path folder = BackupFolder(user);

    // A Pro save from another build, picked as its folder: inspection writes nothing.
    const fs::path source_root = base / "import_source";
    const fs::path source = source_root / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    Bytes source_bytes = MakeSave(2);
    PutU32(source_bytes, kDataSlotGoldOffset, 99);
    Seal(source_bytes);
    WriteBytes(source, source_bytes);
    const auto source_time = fs::last_write_time(source);
    const ImportPlan plan = PlanImport(source_root, save, ImportMode::kPs4);
    CHECK(plan.refused.empty() && plan.inspected.valid && plan.inspected.file == source);
    CHECK(plan.inspected.platform == "PS4" && plan.inspected.region == "Asia/Hong Kong" && plan.inspected.title_id == "CUSA10209");
    CHECK(plan.inspected.data_slots == 3 && plan.inspected.characters == 3 && !plan.unverified_platform);
    CHECK(plan.destination == file && plan.destination_identity == "CUSA10454 / SAVEDATA00 (USA)");
    CHECK(ReadBytes(file) == destination_bytes && CountFiles(folder) == 0);

    // Commit: the destination is backed up first, then replaced byte for byte; the source is never touched.
    const SaveOpResult result = CommitImportPlan(plan, save, folder);
    CHECK(result.ok);
    CHECK(ReadBytes(file) == source_bytes && !fs::exists(file.string() + ".tmp"));
    CHECK(ReadBytes(source) == source_bytes && fs::last_write_time(source) == source_time);
    const std::vector<BackupInfo> backups = ListBackups(folder);
    CHECK(backups.size() == 1 && ReadBytes(backups.front().file) == destination_bytes);
    CHECK(backups.size() == 1 && backups.front().title_id == "CUSA10454" && backups.front().dir_name == "SAVEDATA00");

    // Refused before anything is written.
    const DiscoveredSave now = Only(user);
    CHECK(!PlanImport(source, now, ImportMode::kPs4).refused.empty());                    // identical to the destination
    CHECK(!PlanImport(file, now, ImportMode::kPs4).refused.empty());                      // the destination itself
    CHECK(!PlanImport(base / "nowhere", now, ImportMode::kPs4).refused.empty());
    Bytes corrupt = MakeSave(3);
    corrupt[0] ^= 1;
    WriteBytes(base / "import_corrupt" / "SAVE0.DAT", corrupt);
    const ImportPlan rejected = PlanImport(base / "import_corrupt" / "SAVE0.DAT", now, ImportMode::kPs4);
    CHECK(!rejected.refused.empty() && !rejected.inspected.valid);
    CHECK(!CommitImportPlan(rejected, now, folder).ok);
    WriteBytes(base / "import_named" / "SAVE0.DAT", Bytes(kSaveFileSize, 0));
    CHECK(!PlanImport(base / "import_named", now, ImportMode::kPs4).refused.empty()); // the name alone is not enough
    CHECK(ReadBytes(file) == source_bytes && ListBackups(folder).size() == 1);

    // A failing commit leaves the destination as it was: bytes that stopped validating, a plan for another save, and a
    // destination that became invalid.
    const fs::path next = base / "import_next" / "SAVE0.DAT";
    WriteBytes(next, MakeSave(4));
    ImportPlan tampered = PlanImport(next, now, ImportMode::kPs4);
    CHECK(tampered.refused.empty());
    tampered.inspected.bytes[0x300] ^= 1;
    CHECK(!CommitImportPlan(tampered, now, folder).ok && ReadBytes(file) == source_bytes);
    DiscoveredSave elsewhere = now;
    elsewhere.save0_path = SaveFile(user, "CUSA10454", "SAVEDATA01");
    CHECK(!CommitImportPlan(PlanImport(next, now, ImportMode::kPs4), elsewhere, folder).ok && !fs::exists(elsewhere.save0_path));
    Bytes broken = source_bytes;
    broken[0] ^= 1;
    WriteBytes(file, broken);
    CHECK(!CommitImportPlan(PlanImport(next, now, ImportMode::kPs4), now, folder).ok && ReadBytes(file) == broken);
    WriteBytes(file, source_bytes);
    CHECK(ListBackups(folder).size() == 1);

    // Unknown platform: the file passes every check, so it is offered as what it is.
    WriteBytes(base / "import_plain" / "SAVE0.DAT", MakeSave(6));
    const ImportPlan plain = PlanImport(base / "import_plain" / "SAVE0.DAT", now, ImportMode::kPs4);
    CHECK(plain.refused.empty() && plain.inspected.platform.empty() && plain.inspected.region.empty() && !plain.unverified_platform);
}

// ---- Restore ----

void TestRestore(const fs::path& base) {
    const fs::path user = base / "restore";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes first = MakeSave(1);
    WriteBytes(file, first);
    const fs::path folder = BackupFolder(user);
    const SaveOpResult backup = BackupSave(Only(user), folder);
    CHECK(backup.ok);
    const Bytes second = MakeSave(2);
    WriteBytes(file, second);

    const SaveOpResult restored = RestoreToSave(*backup.backup, Only(user), folder);
    CHECK(restored.ok && ReadBytes(file) == first);
    const std::vector<BackupInfo> after = ListBackups(folder);
    CHECK(after.size() == 2);
    bool current_kept = false;
    for (const BackupInfo& info : after) current_kept = current_kept || ReadBytes(info.file) == second;
    CHECK(current_kept);
    CHECK(Only(user).model.valid() && std::ranges::equal(Only(user).model.bytes(), first));

    // A corrupt backup is refused before anything is backed up or written.
    Bytes corrupt = first;
    corrupt[0x500] ^= 1;
    WriteBytes(backup.backup->file, corrupt);
    CHECK(!RestoreToSave(*backup.backup, Only(user), folder).ok && ReadBytes(file) == first && ListBackups(folder).size() == 2);

    // An invalid destination is never replaced from the page.
    const fs::path bad_user = base / "restore_bad";
    Bytes broken = first;
    broken[0] ^= 1;
    WriteBytes(SaveFile(bad_user, "CUSA10454", "SAVEDATA00"), broken);
    CHECK(!RestoreToSave(after.front(), Only(bad_user), BackupFolder(bad_user)).ok);
    CHECK(ReadBytes(SaveFile(bad_user, "CUSA10454", "SAVEDATA00")) == broken);

    // The page reloads after a replacement and tells the Editor through the shared selection.
    const std::string page = Source("launcher/save_page.cpp");
    const std::string replaced = Body(page, "void SavePage::Replaced(trg::Ui& ui, const char* message) {");
    // The reload waits for the next frame, so the rest of this one never draws from a vector that was just replaced.
    CHECK(replaced.find("++selection_.generation;") < replaced.find("reload_ = true;") && replaced.find("Refresh();") == std::string::npos);
    CHECK(Body(page, "void SavePage::Draw(trg::Ui& ui) {").find("if (std::exchange(reload_, false)) Refresh();") <
          Body(page, "void SavePage::Draw(trg::Ui& ui) {").find("DrawSelector(ui);"));
    CHECK(Body(page, "void SavePage::DrawBackupConfirm(").find("Replaced(ui, ") != std::string::npos);
    CHECK(Body(page, "void SavePage::DrawImportConfirm(").find("Replaced(ui, ") != std::string::npos);
}

// ---- The Editor never writes a stale working copy ----

void TestEditorInvalidation(const fs::path& base) {
    const fs::path user = base / "editor";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes original = MakeSave(1);
    WriteBytes(file, original);
    const fs::path folder = BackupFolder(user);

    // Restore under an open edit.
    const SaveOpResult backup = BackupSave(Only(user), folder);
    SaveEdit edit(Only(user), folder);
    CHECK(edit.working().SetGold(0, 5));
    Bytes changed = MakeSave(2);
    WriteBytes(file, changed);
    CHECK(RestoreToSave(*backup.backup, Only(user), folder).ok && ReadBytes(file) == original);
    CHECK(edit.working().SetGold(0, 5) && edit.dirty());
    // Still matches the bytes it opened on, so this one is allowed; reload to make the next one stale.
    SaveEdit stale(Only(user), folder);
    CHECK(stale.working().SetGold(0, 6));

    // Import under an open edit.
    WriteBytes(base / "editor_src" / "SAVE0.DAT", changed);
    CHECK(CommitImportPlan(PlanImport(base / "editor_src", Only(user), ImportMode::kPs4), Only(user), folder).ok && ReadBytes(file) == changed);
    const size_t backups = ListBackups(folder).size();
    CHECK(stale.Apply() == SaveEdit::Result::kStale);
    CHECK(ReadBytes(file) == changed && ListBackups(folder).size() == backups);
    CHECK(edit.Apply() == SaveEdit::Result::kStale && ReadBytes(file) == changed);

    // A reloaded edit writes, and only gold and the CRC change.
    SaveEdit fresh(Only(user), folder);
    CHECK(fresh.working().SetGold(1, 31337));
    CHECK(fresh.Apply() == SaveEdit::Result::kApplied);
    const Bytes written = ReadBytes(file);
    size_t differing = 0;
    for (size_t i = 4; i < written.size(); ++i) {
        const size_t gold = kDataSlotStride + kDataSlotGoldOffset;
        if (written[i] != changed[i] && (i < gold || i >= gold + 4)) ++differing;
    }
    CHECK(differing == 0 && SaveModel::FromBytes(written).slots()[1].gold == 31337);

    // The Editor follows the shared selection and reloads after a replacement; it never refreshes over pending edits.
    const std::string editor = Source("launcher/editor_page.cpp");
    CHECK(editor.find("const bool replaced = selection_ && selection_->generation != seen_generation_;") != std::string::npos);
    CHECK(editor.find("if ((frame > last_frame_ + 1 || replaced) && !dirty()) refresh_pending_ = true;") != std::string::npos);
    CHECK(Body(editor, "void EditorPage::Refresh() {").find("seen_generation_ = selection_->generation;") != std::string::npos);
    CHECK(editor.find("case SaveEdit::Result::kStale:") != std::string::npos);
    CHECK(Source("launcher/save_edit.cpp").find("return Result::kStale;") <
          Source("launcher/save_edit.cpp").find("CreateBackup(backup_folder_"));
}

// ---- Page source: localization, DPI, no dead UI, no prominent paths ----

void TestPageSource() {
    const std::string page = Source("launcher/save_page.cpp");
    const std::string model = Source("launcher/save_page_model.cpp");
    const std::string portuguese = Source("launcher/localization/pt_br_ui_text.cpp");
    std::set<std::string> rows;
    for (size_t at = 0; at < portuguese.size();) {
        const size_t end = std::min(portuguese.find('\n', at), portuguese.size());
        const std::string line = portuguese.substr(at, end - at);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) rows.insert(line.substr(0, tab));
        at = end + 1;
    }
    // Every visible English text of the page has its Portuguese row; adjacent literals are joined first.
    const std::regex joined("\"\\s*\\n\\s*\"");
    const std::regex literal("\"((?:[^\"\\\\]|\\\\.)*)\"");
    int strings = 0;
    for (const std::string* text : {&page, &model}) {
        const std::string source = std::regex_replace(*text, joined, "");
        for (std::sregex_iterator it(source.begin(), source.end(), literal), end; it != end; ++it) {
            const std::string value = (*it)[1];
            if (value.empty() || !std::isupper(static_cast<unsigned char>(value[0])) ||
                value.find_first_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos || value == "Vita" ||
                value.find(".h") != std::string::npos) {
                continue;
            }
            ++strings;
            if (!rows.count(value)) std::printf("missing PT-BR row: %s\n", value.c_str());
            CHECK(rows.count(value) == 1);
        }
    }
    CHECK(strings > 60);
    const std::string launcher = Source("launcher/launcher.cpp");
    for (const char* text : {"Saves", "Your SAVE0.DAT files: overview, backups, export, import and restore.", "Characters, appearance and inventory.",
                             "Choose the folder to export SAVE0.DAT to", "Choose a SAVE0.DAT or an archive of a PS4 save",
                             "Choose an archive of a PS3 save", "Choose the PS3 save folder (SAVE0.DAT and PARAM.PFD)",
                             "Choose the folder holding SAVE0.DAT", "Dragon's Crown save (SAVE0.DAT)",
                             "Open the folder, then select it. Controller: D-pad to move, A to open, B to cancel."}) {
        CHECK(rows.count(text) == 1 && (launcher.find(text) != std::string::npos || std::string(text) == "Saves"));
    }
    CHECK(portuguese.find("\nSaves\tSaves\n") != std::string::npos);
    CHECK(rows.count("The save changed on disk since the Editor loaded it, so the changes were not written. The Editor reloaded the save."));

    // Sizes follow the launcher's DPI scale; the rest is TRG rows.
    const std::regex pixels("([1-9][0-9]*\\.[0-9]+|0\\.[0-9]*[1-9][0-9]*)f");
    for (std::sregex_iterator it(page.begin(), page.end(), pixels), end; it != end; ++it) {
        const std::string after = page.substr(size_t(it->position() + it->length()), 13);
        CHECK(after == " * ui.scale()");
    }
    for (const char* row : {"ui.StatusCard(", "ui.InfoGrid(", "ui.Heading(\"Overview\")", "ui.ButtonRow(\"Create backup\"",
                            "ui.Heading(\"Details\")"}) {
        CHECK(page.find(row) != std::string::npos);
    }
    CHECK(page.find("PushStyleVar(ImGuiStyleVar_") == std::string::npos && page.find("ImGui::GetStyle()") == std::string::npos);

    // Paths only in the details section and the confirmations that name the files involved.
    CHECK(page.find("Utf8(save.save0_path)") == page.rfind("Utf8(save.save0_path)"));
    CHECK(Body(page, "void SavePage::DrawDetails(").find("Utf8(save.save0_path)") != std::string::npos);
    for (const char* future : {"Inventory", "Equipment", "Play as Enemy", "Future"}) CHECK(page.find(future) == std::string::npos);

    // The page only reads the selected save unless an action replaces it, and never moves anything.
    CHECK(model.find("rename(") == std::string::npos && model.find("remove(") == std::string::npos);
    CHECK(page.find("rename(") == std::string::npos && page.find("remove(") == std::string::npos);
    CHECK(page.find("WriteSaveAtomically") == std::string::npos && page.find("ofstream") == std::string::npos);
}

// ---- Synthetic sources: PARAM.SFO, encrypted-looking payloads and ZIP archives ----

Bytes SyntheticSfo(const std::vector<std::pair<std::string, std::string>>& entries) {
    Bytes keys, data, table;
    for (const auto& [key, value] : entries) {
        const uint16_t key_offset = uint16_t(keys.size());
        const uint32_t data_offset = uint32_t(data.size());
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        const uint32_t length = uint32_t(value.size() + 1);
        data.insert(data.end(), value.begin(), value.end());
        data.push_back(0);
        const uint16_t format = 0x0204;
        for (const uint16_t v : {key_offset, format}) {
            table.push_back(uint8_t(v));
            table.push_back(uint8_t(v >> 8));
        }
        for (const uint32_t v : {length, length, data_offset}) {
            for (int i = 0; i < 4; ++i) table.push_back(uint8_t(v >> (8 * i)));
        }
    }
    Bytes sfo = {0, 'P', 'S', 'F', 1, 1, 0, 0};
    const uint32_t key_table = uint32_t(20 + table.size());
    const uint32_t data_table = key_table + uint32_t(keys.size());
    for (const uint32_t v : {key_table, data_table, uint32_t(entries.size())}) {
        for (int i = 0; i < 4; ++i) sfo.push_back(uint8_t(v >> (8 * i)));
    }
    sfo.insert(sfo.end(), table.begin(), table.end());
    sfo.insert(sfo.end(), keys.begin(), keys.end());
    sfo.insert(sfo.end(), data.begin(), data.end());
    return sfo;
}

Bytes RandomBytes(size_t size, uint32_t seed) {
    Bytes bytes(size);
    uint32_t state = seed | 1u;
    for (uint8_t& byte : bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = uint8_t(state >> 11);
    }
    return bytes;
}

uint32_t Crc32Of(const Bytes& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Le16(Bytes& out, uint32_t v) {
    out.push_back(uint8_t(v));
    out.push_back(uint8_t(v >> 8));
}

void Le32(Bytes& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

struct ZipSpec {
    std::string name;
    Bytes data;
    uint16_t method = 0;           // 0 stored, 8 deflate (written as stored deflate blocks)
    uint16_t flags = 0;
    int64_t declared_size = -1;    // overrides the size recorded for the entry
    int64_t declared_crc = -1;
};

Bytes BuildZip(const std::vector<ZipSpec>& specs) {
    Bytes zip, directory;
    for (const ZipSpec& spec : specs) {
        Bytes payload = spec.data;
        if (spec.method == 8) {
            payload.clear();
            size_t at = 0;
            do {
                const size_t chunk = std::min<size_t>(spec.data.size() - at, 65535);
                payload.push_back(at + chunk == spec.data.size() ? 1 : 0);
                Le16(payload, uint32_t(chunk));
                Le16(payload, uint32_t(~chunk) & 0xFFFF);
                payload.insert(payload.end(), spec.data.begin() + at, spec.data.begin() + at + chunk);
                at += chunk;
            } while (at < spec.data.size());
        }
        const uint32_t offset = uint32_t(zip.size());
        const uint32_t crc = spec.declared_crc >= 0 ? uint32_t(spec.declared_crc) : Crc32Of(spec.data);
        const uint32_t size = spec.declared_size >= 0 ? uint32_t(spec.declared_size) : uint32_t(spec.data.size());
        Le32(zip, 0x04034b50);
        Le16(zip, 20);
        Le16(zip, spec.flags);
        Le16(zip, spec.method);
        Le32(zip, 0);
        Le32(zip, crc);
        Le32(zip, uint32_t(payload.size()));
        Le32(zip, size);
        Le16(zip, uint32_t(spec.name.size()));
        Le16(zip, 0);
        zip.insert(zip.end(), spec.name.begin(), spec.name.end());
        zip.insert(zip.end(), payload.begin(), payload.end());

        Le32(directory, 0x02014b50);
        Le16(directory, 20);
        Le16(directory, 20);
        Le16(directory, spec.flags);
        Le16(directory, spec.method);
        Le32(directory, 0);
        Le32(directory, crc);
        Le32(directory, uint32_t(payload.size()));
        Le32(directory, size);
        Le16(directory, uint32_t(spec.name.size()));
        Le16(directory, 0);
        Le16(directory, 0);
        Le16(directory, 0);
        Le16(directory, 0);
        Le32(directory, 0);
        Le32(directory, offset);
        directory.insert(directory.end(), spec.name.begin(), spec.name.end());
    }
    const uint32_t directory_offset = uint32_t(zip.size());
    zip.insert(zip.end(), directory.begin(), directory.end());
    Le32(zip, 0x06054b50);
    Le16(zip, 0);
    Le16(zip, 0);
    Le16(zip, uint32_t(specs.size()));
    Le16(zip, uint32_t(specs.size()));
    Le32(zip, uint32_t(directory.size()));
    Le32(zip, directory_offset);
    Le16(zip, 0);
    return zip;
}

std::string Hash(const fs::path& path) {
    const Bytes bytes = ReadBytes(path);
    return std::to_string(Crc32Of(bytes)) + ":" + std::to_string(bytes.size());
}

// A PS3 save folder as the console keeps it: encrypted SAVE0.DAT, PARAM.SFO and PARAM.PFD.
void WritePs3Folder(const fs::path& folder, const char* title_id, const Bytes& save0, bool with_sfo = true) {
    WriteBytes(folder / "SAVE0.DAT", save0);
    WriteBytes(folder / "PARAM.PFD", Bytes{0, 0, 0, 0, 'P', 'F', 'D', 'B', 1, 2, 3});
    if (with_sfo) {
        WriteBytes(folder / "PARAM.SFO",
                   SyntheticSfo({{"SAVEDATA_DIRECTORY", std::string(title_id) + "-AUTO_0-"}, {"TITLE", "Dragon's Crown Save Data"}}));
    }
}

void TestTitleTables() {
    const struct {
        const char* id;
        const char* platform;
        const char* region;
    } known[] = {{"BLUS30767", "PS3", "USA"},  {"NPUB31235", "PS3", "USA"}, {"NPEB01836", "PS3", "Europe"},
                 {"BLES01950", "PS3", "Europe"}, {"BLJM61041", "PS3", "Japan"}, {"PCSE00019", "Vita", "USA"},
                 {"PCSB00408", "Vita", "Europe"}};
    for (const auto& item : known) {
        CHECK(std::string(ImportPlatformOf(item.id)) == item.platform && ImportRegionOf(item.id) == item.region);
    }
    CHECK(std::string(ImportPlatformOf("PCSE00899")) == "" && ImportPlatformOf("") != nullptr);
    CHECK(std::string(ImportPlatformOf("CUSA10454")) == ""); // a Pro id is not a legacy id

    const std::optional<SfoInfo> ps3 = ParseParamSfo(SyntheticSfo({{"SAVEDATA_DIRECTORY", "BLUS30767-AUTO_0-"}}));
    CHECK(ps3 && TitleIdOfSfo(*ps3) == "BLUS30767");
    const std::optional<SfoInfo> vita = ParseParamSfo(SyntheticSfo({{"PARENT_DIRECTORY", "/PCSE00019"}, {"CATEGORY", "sd"}}));
    CHECK(vita && TitleIdOfSfo(*vita) == "PCSE00019" && vita->text.at("CATEGORY") == "sd");
    const std::optional<SfoInfo> odd = ParseParamSfo(SyntheticSfo({{"SAVEDATA_DIRECTORY", "../../etc"}, {"PARENT_DIRECTORY", "PCSE0001"}}));
    CHECK(odd && TitleIdOfSfo(*odd).empty());

    Bytes sfo = SyntheticSfo({{"SAVEDATA_DIRECTORY", "BLUS30767-AUTO_0-"}});
    CHECK(!ParseParamSfo(Bytes{}) && !ParseParamSfo(Bytes(10, 0)));
    Bytes wrong_magic = sfo;
    wrong_magic[1] = 'X';
    CHECK(!ParseParamSfo(wrong_magic));
    Bytes many = sfo;
    many[16] = 0xFF; // entry count far beyond the file
    CHECK(!ParseParamSfo(many));
    Bytes wild = sfo;
    wild[20 + 12] = 0xFF; // data offset beyond the file: the entry is skipped, nothing is read
    wild[20 + 13] = 0xFF;
    const std::optional<SfoInfo> skipped = ParseParamSfo(wild);
    CHECK(skipped && skipped->text.empty());
    Bytes truncated(sfo.begin(), sfo.begin() + 24);
    CHECK(!ParseParamSfo(truncated));

    CHECK(ByteEntropy(Bytes(1000, 0)) == 0.0 && ByteEntropy(Bytes{}) == 0.0);
    CHECK(ByteEntropy(RandomBytes(1 << 16, 7)) > 7.9 && ByteEntropy(MakeSave()) < 3.0);
}

// ---- Sources: file, folder, PS3 and Vita layouts, archives ----

void TestImportSources(const fs::path& base) {
    const fs::path user = base / "sources_user";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave(1));
    const DiscoveredSave destination = Only(user);
    const fs::path root = base / "sources";
    const Bytes good = MakeSave(2);

    // A raw SAVE0.DAT, its folder, and a higher-level folder holding the save folder.
    WriteBytes(root / "raw" / "SAVE0.DAT", good);
    ImportResult raw = InspectImport(root / "raw" / "SAVE0.DAT");
    CHECK(raw.valid && raw.block == ImportBlock::kNone && raw.source == ImportSource::kSaveFile && raw.bytes == good);
    CHECK(raw.platform.empty() && !raw.conversion_required && raw.format == "Dragon's Crown Pro layout" && raw.data_slots == 3);
    const ImportResult folder = InspectImport(root / "raw");
    CHECK(folder.valid && folder.source == ImportSource::kFolder && folder.bytes == good && folder.candidates == 1);
    WriteBytes(root / "nested" / "one" / "two" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT", good);
    const ImportResult nested = InspectImport(root / "nested");
    CHECK(nested.valid && nested.title_id == "CUSA10209" && nested.platform == "PS4" && nested.region == "Asia/Hong Kong");
    WriteBytes(root / "tooDeep" / "a" / "b" / "c" / "d" / "e" / "SAVE0.DAT", good);
    CHECK(InspectImport(root / "tooDeep").block == ImportBlock::kNoSave);

    // Nothing to import, and no way to guess between different saves.
    fs::create_directories(root / "empty");
    CHECK(InspectImport(root / "empty").block == ImportBlock::kNoSave && !InspectImport(root / "empty").valid);
    CHECK(InspectImport(root / "nowhere").block == ImportBlock::kUnreadable);
    WriteBytes(root / "two" / "a" / "SAVE0.DAT", MakeSave(3));
    WriteBytes(root / "two" / "b" / "SAVE0.DAT", MakeSave(4));
    const ImportResult ambiguous = InspectImport(root / "two");
    CHECK(ambiguous.block == ImportBlock::kAmbiguous && !ambiguous.valid && ambiguous.candidates == 2 && ambiguous.bytes.empty());
    WriteBytes(root / "twins" / "a" / "SAVE0.DAT", MakeSave(3));
    WriteBytes(root / "twins" / "b" / "SAVE0.DAT", MakeSave(3));
    CHECK(InspectImport(root / "twins").valid && InspectImport(root / "twins").candidates == 2);

    // A file that is not a save, however it is named, and a file far too large.
    WriteBytes(root / "named" / "SAVE0.DAT", Bytes(kSaveFileSize, 0));
    CHECK(InspectImport(root / "named" / "SAVE0.DAT").block == ImportBlock::kInvalidSave);
    WriteBytes(root / "short" / "SAVE0.DAT", Bytes(100, 1));
    const ImportResult too_short = InspectImport(root / "short" / "SAVE0.DAT");
    CHECK(too_short.block == ImportBlock::kInvalidSave && !too_short.errors.empty());
    Bytes bad_crc = good;
    bad_crc[0x500] ^= 1;
    WriteBytes(root / "badcrc" / "SAVE0.DAT", bad_crc);
    const ImportResult corrupt = InspectImport(root / "badcrc" / "SAVE0.DAT");
    CHECK(corrupt.block == ImportBlock::kInvalidSave && corrupt.errors.front().find("CRC") != std::string::npos);
    WriteBytes(root / "huge" / "SAVE0.DAT", Bytes((16u << 20) + 1, 0));
    CHECK(InspectImport(root / "huge" / "SAVE0.DAT").block == ImportBlock::kTooLarge);

    // PS3: an encrypted container that cannot be read is refused; a Pro layout under a PS3 title is not a PS3 layout.
    const Bytes encrypted = RandomBytes(kSaveFileSize + 8, 11);
    WritePs3Folder(root / "ps3" / "BLUS30767-AUTO_0-", "BLUS30767", encrypted);
    const ImportResult ps3 = InspectImport(root / "ps3");
    CHECK(!ps3.valid && ps3.block == ImportBlock::kPs3PfdInvalid && ps3.platform == "PS3" && ps3.region == "USA" && ps3.encrypted);
    CHECK(ps3.title_id == "BLUS30767" && ps3.has_param_sfo && ps3.has_param_pfd && ps3.conversion_required);
    CHECK(ps3.format == "PS3 container, encrypted" && ps3.pfd_state == "Invalid" && ps3.bytes.empty());
    WritePs3Folder(root / "ps3_nosfo" / "BLJM61041-AUTO_0-", "BLJM61041", encrypted, false);
    const ImportResult japan = InspectImport(root / "ps3_nosfo");
    CHECK(japan.block == ImportBlock::kPs3PfdInvalid && japan.platform == "PS3" && japan.region == "Japan" && !japan.has_param_sfo);
    WriteBytes(root / "ps3_bare" / "SAVE0.DAT", encrypted); // no folder, no SFO: still recognized by its size and entropy
    CHECK(InspectImport(root / "ps3_bare" / "SAVE0.DAT").block == ImportBlock::kPs3NeedsPfd);
    WritePs3Folder(root / "ps3_plain" / "NPUB31235-AUTO_0-", "NPUB31235", good);
    const ImportResult layout = InspectImport(root / "ps3_plain");
    CHECK(!layout.valid && layout.block == ImportBlock::kPs3UnsupportedLayout && layout.conversion_required && layout.platform == "PS3");

    // Vita: the SFO and sce_sys name the title. A readable save is accepted as unverified; an encrypted one is refused.
    const fs::path vita = root / "vita" / "PCSE00019_SLOT0";
    WriteBytes(vita / "SAVE0.DAT", good);
    WriteBytes(vita / "sce_sys" / "param.sfo", SyntheticSfo({{"PARENT_DIRECTORY", "/PCSE00019"}, {"CATEGORY", "sd"}}));
    WriteBytes(vita / "sce_sys" / "sealedkey", Bytes(80, 1));
    const ImportResult vita_plain = InspectImport(root / "vita");
    CHECK(vita_plain.valid && vita_plain.platform == "Vita" && vita_plain.region == "USA" && vita_plain.title_id == "PCSE00019");
    CHECK(!vita_plain.conversion_required && vita_plain.has_param_sfo && vita_plain.bytes == good);
    const fs::path vita_locked = root / "vita_locked" / "PCSE00019_SLOT0";
    WriteBytes(vita_locked / "SAVE0.DAT", RandomBytes(kSaveFileSize, 12));
    WriteBytes(vita_locked / "sce_sys" / "param.sfo", SyntheticSfo({{"PARENT_DIRECTORY", "/PCSE00019"}}));
    const ImportResult locked = InspectImport(root / "vita_locked");
    CHECK(!locked.valid && locked.block == ImportBlock::kEncryptedVita && locked.platform == "Vita");
    const ImportPlan vita_plan = PlanImport(root / "vita", destination, ImportMode::kPs4);
    CHECK(vita_plan.refused.empty() && vita_plan.unverified_platform);
    CHECK(PlanImport(root / "ps3", destination, ImportMode::kPs4).unverified_platform == false && !PlanImport(root / "ps3", destination, ImportMode::kPs4).refused.empty());

    // Plans and commits: a refused source never writes; the source stays as it was.
    const std::string before = Hash(root / "ps3" / "BLUS30767-AUTO_0-" / "SAVE0.DAT");
    const SaveOpResult refused = CommitImportPlan(PlanImport(root / "ps3", destination, ImportMode::kPs4), destination, BackupFolder(user));
    CHECK(!refused.ok && ReadBytes(file) == MakeSave(1) && CountFiles(BackupFolder(user)) == 0);
    CHECK(Hash(root / "ps3" / "BLUS30767-AUTO_0-" / "SAVE0.DAT") == before);
    const std::string source_before = Hash(root / "raw" / "SAVE0.DAT");
    const SaveOpResult done = CommitImportPlan(PlanImport(root / "raw", destination, ImportMode::kPs4), destination, BackupFolder(user));
    CHECK(done.ok && ReadBytes(file) == good && Hash(root / "raw" / "SAVE0.DAT") == source_before);
    CHECK(ListBackups(BackupFolder(user)).size() == 1 && ReadBytes(ListBackups(BackupFolder(user))[0].file) == MakeSave(1));

    // The destination changed after the page loaded it: the import is refused and nothing is touched.
    const DiscoveredSave loaded = Only(user);
    Bytes other = MakeSave(9);
    WriteBytes(file, other);
    const SaveOpResult stale = CommitImportPlan(PlanImport(root / "twins", loaded, ImportMode::kPs4), loaded, BackupFolder(user));
    CHECK(!stale.ok && stale.stale && ReadBytes(file) == other && ListBackups(BackupFolder(user)).size() == 1);
}

void TestArchives(const fs::path& base) {
    const fs::path root = base / "archives";
    const Bytes good = MakeSave(5);
    const std::filesystem::path name = "save.zip";

    // Stored and deflated entries read the same; the SFO beside SAVE0.DAT names the title.
    for (const uint16_t method : {uint16_t(0), uint16_t(8)}) {
        const Bytes zip = BuildZip({{"Saves/BLUS30767-AUTO_0-/", {}, 0}, {"Saves/CUSA10454/SAVEDATA00/SAVE0.DAT", good, method},
                                    {"Saves/CUSA10454/SAVEDATA00/icon.png", Bytes(40, 1), method}});
        const ImportResult result = InspectZip(zip, name);
        CHECK(result.valid && result.source == ImportSource::kZip && result.bytes == good && result.title_id == "CUSA10454");
        CHECK(result.entry == "Saves/CUSA10454/SAVEDATA00/SAVE0.DAT" && result.file == name && result.candidates == 1);
    }
    const Bytes vita_zip = BuildZip({{"PCSE00019_SLOT0/SAVE0.DAT", good, 8},
                                     {"PCSE00019_SLOT0/sce_sys/param.sfo", SyntheticSfo({{"PARENT_DIRECTORY", "/PCSE00019"}}), 8}});
    const ImportResult vita = InspectZip(vita_zip, name);
    CHECK(vita.valid && vita.platform == "Vita" && vita.region == "USA" && vita.has_param_sfo);
    const Bytes ps3_zip = BuildZip({{"BLUS30767-AUTO_0-/SAVE0.DAT", RandomBytes(kSaveFileSize + 8, 5), 0},
                                    {"BLUS30767-AUTO_0-/PARAM.PFD", Bytes{1, 2, 3}, 0},
                                    {"BLUS30767-AUTO_0-/PARAM.SFO", SyntheticSfo({{"SAVEDATA_DIRECTORY", "BLUS30767-AUTO_0-"}}), 0}});
    const ImportResult ps3 = InspectZip(ps3_zip, name);
    CHECK(!ps3.valid && ps3.block == ImportBlock::kPs3PfdInvalid && ps3.platform == "PS3" && ps3.has_param_pfd);
    const Bytes number_zip = BuildZip({{"1 - The Adventure Begins/BLUS30767-AUTO_0-/SAVE0.DAT", RandomBytes(kSaveFileSize + 8, 6), 0}});
    CHECK(InspectZip(number_zip, name).title_id == "BLUS30767");

    // Names that leave the archive, or that are not names at all.
    for (const char* unsafe : {"../evil/SAVE0.DAT", "a/../../SAVE0.DAT", "..\\evil\\SAVE0.DAT", "/etc/SAVE0.DAT", "\\evil\\SAVE0.DAT",
                               "C:/evil/SAVE0.DAT", "C:\\evil\\SAVE0.DAT", "x/../SAVE0.DAT"}) {
        const ImportResult result = InspectZip(BuildZip({{unsafe, good, 0}}), name);
        CHECK(!result.valid && result.block == ImportBlock::kArchiveUnsafe && result.bytes.empty());
    }
    CHECK(!SafeArchivePath(std::string("a\0b", 3), 260) && !SafeArchivePath("", 260) && !SafeArchivePath(std::string(300, 'a'), 260));
    CHECK(SafeArchivePath("a\\b/c.dat", 260) == std::string("a/b/c.dat") && SafeArchivePath("..a/b..", 260));
    // A refused entry anywhere refuses the whole archive, even when the save itself is fine.
    CHECK(InspectZip(BuildZip({{"ok/SAVE0.DAT", good, 0}, {"../x.txt", Bytes(1, 1), 0}}), name).block == ImportBlock::kArchiveUnsafe);

    // Limits: entry count, file size, total size, declared sizes that lie, and deflate output beyond the declared size.
    ZipLimits few;
    few.max_entries = 2;
    CHECK(InspectZip(BuildZip({{"a/SAVE0.DAT", good, 0}, {"b.txt", Bytes(1, 1), 0}, {"c.txt", Bytes(1, 1), 0}}), name, few).block ==
          ImportBlock::kArchiveLimits);
    ZipLimits small;
    small.max_file = 1000;
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 0}}), name, small).block == ImportBlock::kArchiveLimits);
    ZipLimits cap;
    cap.max_total = kSaveFileSize + 100;
    CHECK(InspectZip(BuildZip({{"a/SAVE0.DAT", good, 0}, {"a/PARAM.SFO", Bytes(200, 1), 0}}), name, cap).block == ImportBlock::kArchiveLimits);
    ZipLimits count;
    count.max_extracted = 1;
    CHECK(InspectZip(BuildZip({{"a/SAVE0.DAT", good, 0}, {"a/PARAM.SFO", Bytes(20, 1), 0}}), name, count).block == ImportBlock::kArchiveLimits);
    ZipLimits tiny_archive;
    tiny_archive.max_archive = 64;
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 0}}), name, tiny_archive).block == ImportBlock::kArchiveLimits);
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 8, 0, int64_t(20) << 20}}), name).block == ImportBlock::kArchiveLimits);
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 8, 0, 100}}), name).block == ImportBlock::kArchiveLimits); // inflates beyond what it declared
    CHECK(!InspectZip(BuildZip({{"SAVE0.DAT", good, 0, 0, 100}}), name).valid);                                // stored: size and data disagree

    // Encrypted, unsupported and damaged archives.
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 0, 0x0001}}), name).block == ImportBlock::kArchiveEncrypted);
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 12}}), name).block == ImportBlock::kArchiveInvalid);
    CHECK(InspectZip(BuildZip({{"SAVE0.DAT", good, 0, 0, -1, 12345}}), name).block == ImportBlock::kArchiveInvalid); // wrong CRC-32
    Bytes truncated = BuildZip({{"SAVE0.DAT", good, 0}});
    truncated.resize(truncated.size() - 30);
    CHECK(InspectZip(truncated, name).block == ImportBlock::kArchiveInvalid);
    CHECK(InspectZip(Bytes(10, 1), name).block == ImportBlock::kArchiveInvalid &&
          InspectZip(std::span<const uint8_t>(), name).block == ImportBlock::kArchiveInvalid);
    Bytes bad_offset = BuildZip({{"SAVE0.DAT", good, 0}});
    for (size_t i = 0; i + 4 < bad_offset.size(); ++i) {
        if (bad_offset[i] == 0x50 && bad_offset[i + 1] == 0x4b && bad_offset[i + 2] == 1 && bad_offset[i + 3] == 2) {
            bad_offset[i + 42] = 0xFF; // local header offset far outside the archive
            bad_offset[i + 43] = 0xFF;
            break;
        }
    }
    CHECK(InspectZip(bad_offset, name).block == ImportBlock::kArchiveInvalid);

    // No save, several different saves, identical copies.
    CHECK(InspectZip(BuildZip({{"readme.txt", Bytes(5, 'x'), 0}}), name).block == ImportBlock::kNoSave);
    const ImportResult two = InspectZip(BuildZip({{"a/SAVE0.DAT", MakeSave(6), 0}, {"b/SAVE0.DAT", MakeSave(7), 0}}), name);
    CHECK(!two.valid && two.block == ImportBlock::kAmbiguous && two.candidates == 2 && two.bytes.empty());
    CHECK(InspectZip(BuildZip({{"a/SAVE0.DAT", MakeSave(6), 0}, {"b/SAVE0.DAT", MakeSave(6), 8}}), name).valid);

    // Through the file path: detected by content, never by extension, and the original is not modified.
    WriteBytes(root / "good.zip", BuildZip({{"CUSA10454/SAVEDATA00/SAVE0.DAT", good, 8}}));
    const std::string zip_before = Hash(root / "good.zip");
    const ImportResult from_disk = InspectImport(root / "good.zip");
    CHECK(from_disk.valid && from_disk.source == ImportSource::kZip && from_disk.bytes == good && from_disk.file == root / "good.zip");
    CHECK(Hash(root / "good.zip") == zip_before && !fs::exists(root / "SAVE0.DAT") && CountTree(root) == 1);
    WriteBytes(root / "renamed.dat", BuildZip({{"SAVE0.DAT", good, 0}}));
    CHECK(InspectImport(root / "renamed.dat").source == ImportSource::kZip);
    WriteBytes(root / "plain.zip", good); // a save named .zip is still a save
    CHECK(InspectImport(root / "plain.zip").source == ImportSource::kSaveFile && InspectImport(root / "plain.zip").valid);
    WriteBytes(root / "pk.dat", Bytes{'P', 'K', 0x01, 0x01, 9, 9, 9, 9, 9, 9});
    CHECK(InspectImport(root / "pk.dat").source == ImportSource::kSaveFile && InspectImport(root / "pk.dat").block == ImportBlock::kInvalidSave);

    // RAR: recognized by its signature, explained, never opened.
    for (const Bytes& rar : {Bytes{'R', 'a', 'r', '!', 0x1a, 0x07, 0x00, 1, 2, 3}, Bytes{'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00, 4, 5}}) {
        WriteBytes(root / "set.rar", rar);
        const ImportResult result = InspectImport(root / "set.rar");
        CHECK(!result.valid && result.block == ImportBlock::kRarArchive && result.source == ImportSource::kRar && result.bytes.empty());
        CHECK(std::string(ImportBlockText(result.block)).find("Extract it first") != std::string::npos);
    }
    WriteBytes(root / "fake.rar", good);
    CHECK(InspectImport(root / "fake.rar").valid);
}

// ---- PS3 and Vita: sources, diagnostics, no conversion ----

void TestPlatformDiagnostics() {
    const std::string source = Source("launcher/save_import.cpp");
    CHECK(source.find("result.platform == \"PS3\"") != std::string::npos);
    CHECK(source.find("ByteSwap") == std::string::npos && source.find("bswap") == std::string::npos &&
          source.find("byteswap") == std::string::npos);
    for (const ImportBlock block : {ImportBlock::kUnreadable, ImportBlock::kTooLarge, ImportBlock::kNoSave, ImportBlock::kAmbiguous,
                                    ImportBlock::kRarArchive, ImportBlock::kArchiveInvalid, ImportBlock::kArchiveUnsafe,
                                    ImportBlock::kArchiveLimits, ImportBlock::kArchiveEncrypted, ImportBlock::kPs3NeedsPfd,
                                    ImportBlock::kPs3PfdInvalid, ImportBlock::kPs3UnknownTitle, ImportBlock::kPs3HashMismatch,
                                    ImportBlock::kPs3Undecryptable, ImportBlock::kPs3UnsupportedLayout, ImportBlock::kEncryptedVita,
                                    ImportBlock::kInvalidSave}) {
        CHECK(*ImportBlockText(block) != '\0');
    }
    CHECK(*ImportBlockText(ImportBlock::kNone) == '\0');
    const std::string header = Source("launcher/save_import.h");
    CHECK(header.find("share one") == std::string::npos && header.find("no conversion to the Pro layout is") != std::string::npos);

    const std::string portuguese = Source("launcher/localization/pt_br_ui_text.cpp");
    std::set<std::string> rows;
    for (size_t at = 0; at < portuguese.size();) {
        const size_t end = std::min(portuguese.find('\n', at), portuguese.size());
        const std::string line = portuguese.substr(at, end - at);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos) rows.insert(line.substr(0, tab));
        at = end + 1;
    }
    for (const char* text : {"PS3 container, encrypted", "PS3 layout, big-endian", "PS3 layout, big-endian (decrypted)", "PS Vita container, encrypted",
                             "Not a Dragon's Crown Pro save", "Dragon's Crown Pro layout"}) {
        CHECK(rows.count(text) == 1);
    }
    for (int i = 1; i <= int(ImportBlock::kInvalidSave); ++i) CHECK(rows.count(ImportBlockText(ImportBlock(i))) == 1);
}

// ---- SaveEdit and the SaveModel setters the Editor uses ----

void TestEditSaveFields(const fs::path& base) {
    const fs::path user = base / "edit_fields";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    const Bytes original = MakeSave(3);
    WriteBytes(file, original);
    const fs::path folder = BackupFolder(user);

    // Every field: minimum, normal, maximum, refusal outside the range; nothing but the field and the CRC changes.
    const struct {
        const char* name;
        std::function<bool(SaveModel&, int)> set;
        std::function<int(const SaveModel&)> get;
        std::vector<int> accepted;
        std::vector<int> refused;
        std::vector<size_t> offsets;
    } fields[] = {
        {"gold", [](SaveModel& m, int v) { return m.SetGold(0, uint32_t(v)); },
         [](const SaveModel& m) { return int(m.slots()[0].gold); }, {0, 1234567, int(kMaxGold)}, {-1, int(kMaxGold) + 1},
         {kDataSlotGoldOffset, kDataSlotGoldOffset + 1, kDataSlotGoldOffset + 2, kDataSlotGoldOffset + 3}},
        {"level", [](SaveModel& m, int v) { return m.SetLevel(0, 0, v); },
         [](const SaveModel& m) { return SaveModel::DisplayedLevel(m.Character(0, 0)->level_stored); }, {kMinLevel, 15, 99, kMaxLevel},
         {0, -5, kMaxLevel + 1}, {RecordOffset(0, 0) + kCharLevel}},
        {"skill points", [](SaveModel& m, int v) { return m.SetSkillPoints(0, 0, v); },
         [](const SaveModel& m) { return int(m.Character(0, 0)->skill_points); }, {0, 15, kMaxSkillPoints}, {-1, kMaxSkillPoints + 1},
         {RecordOffset(0, 0) + kCharSkillPoints, RecordOffset(0, 0) + kCharSkillPoints + 1}},
    };
    for (const auto& field : fields) {
        for (const int value : field.accepted) {
            SaveEdit edit(Only(user), folder);
            CHECK(field.set(edit.working(), value) && field.get(edit.working()) == value);
            const bool changed = field.get(edit.persisted()) != value;
            CHECK(edit.dirty() == changed);
            if (!changed) continue;
            CHECK(edit.Apply() == SaveEdit::Result::kApplied);
            const Bytes written = ReadBytes(file);
            std::vector<std::string> problems;
            CHECK(ValidateSave(written, problems) && field.get(SaveModel::FromBytes(written)) == value);
            size_t outside = 0;
            const Bytes before = ReadBytes(ListBackups(folder).back().file);
            for (size_t i = 4; i < written.size(); ++i) {
                if (written[i] != before[i] && std::find(field.offsets.begin(), field.offsets.end(), i) == field.offsets.end()) ++outside;
            }
            CHECK(outside == 0 && written.size() == original.size());
            WriteBytes(file, original);
            fs::remove_all(folder);
        }
        for (const int value : field.refused) {
            SaveEdit edit(Only(user), folder);
            const Bytes before(edit.working().bytes().begin(), edit.working().bytes().end());
            CHECK(!field.set(edit.working(), value));
            CHECK(std::ranges::equal(edit.working().bytes(), before) && !edit.dirty());
        }
    }

    // HP: current never above max; a pair outside what a stored u16 holds cannot be expressed.
    for (const auto& [current, maximum] : {std::pair<int, int>{0, 0}, {349, 349}, {1, 65535}, {65535, 65535}}) {
        SaveEdit edit(Only(user), folder);
        CHECK(edit.working().SetHp(0, 0, uint16_t(current), uint16_t(maximum)));
        CHECK(edit.working().Character(0, 0)->hp_current == current && edit.working().Character(0, 0)->hp_max == maximum);
    }
    SaveEdit hp(Only(user), folder);
    CHECK(!hp.working().SetHp(0, 0, 400, 399) && !hp.working().SetHp(0, 99, 1, 1) && !hp.working().SetHp(9, 0, 1, 1));
    CHECK(!hp.working().SetLevel(0, 5, 9) && !hp.working().SetGold(7, 1) && !hp.dirty()); // empty record, invalid slot
    const HpValues lowered = WithMaxHp({349, 349}, 100);
    CHECK(lowered.current == 100 && lowered.max == 100 && WithCurrentHp({349, 349}, 9999).current == 349);

    // Several edits at once, one backup, one atomic write, a CRC that validates, unknown bytes untouched.
    fs::remove_all(folder);
    SaveEdit edit(Only(user), folder);
    const uint16_t kept_max = edit.working().Character(0, 1)->hp_max;
    CHECK(edit.working().SetGold(2, 4242) && edit.working().SetLevel(0, 1, 77) && edit.working().SetSkillPoints(0, 1, 5) &&
          edit.working().SetHp(0, 1, 1, kept_max));
    CHECK(edit.Apply() == SaveEdit::Result::kApplied && ListBackups(folder).size() == 1);
    CHECK(ReadBytes(ListBackups(folder)[0].file) == original);
    const Bytes after = ReadBytes(file);
    std::vector<std::string> problems;
    CHECK(ValidateSave(after, problems) && !fs::exists(file.string() + ".tmp"));
    size_t unknown_changed = 0;
    for (size_t i = 4; i < after.size(); ++i) {
        const size_t record = RecordOffset(0, 1), gold = 2 * kDataSlotStride + kDataSlotGoldOffset;
        const bool known = i == record + kCharLevel || i == record + kCharSkillPoints || i == record + kCharSkillPoints + 1 ||
                           i == record + kCharHpCurrent || i == record + kCharHpCurrent + 1 || i == record + kCharHpMax ||
                           i == record + kCharHpMax + 1 || (i >= gold && i < gold + 4);
        if (after[i] != original[i] && !known) ++unknown_changed;
    }
    CHECK(unknown_changed == 0);
    for (size_t i = 0x100; i < 0x100 + 64; ++i) CHECK(after[i] == original[i]);
    const SaveModel reread = SaveModel::FromBytes(after);
    CHECK(reread.slots()[2].gold == 4242 && SaveModel::DisplayedLevel(reread.Character(0, 1)->level_stored) == 77 &&
          reread.Character(0, 1)->skill_points == 5 && reread.Character(0, 1)->hp_current == 1 && reread.Character(0, 1)->hp_max == kept_max);
    CHECK(reread.Character(0, 0)->name == "Eppie" && reread.Character(0, 1)->name == "Allie");

    // Discard restores; nothing is written for an unchanged edit; a second apply has nothing to do.
    SaveEdit discard(Only(user), folder);
    CHECK(discard.working().SetGold(1, 55) && discard.dirty());
    discard.Discard();
    CHECK(!discard.dirty() && discard.Apply() == SaveEdit::Result::kNothingToApply && ReadBytes(file) == after);
    CHECK(edit.Apply() == SaveEdit::Result::kNothingToApply);
}

void TestEditSaveSafety(const fs::path& base) {
    const fs::path user = base / "edit_safety";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave(4));
    const fs::path folder = BackupFolder(user);

    // The save changes on disk (the Editor, an import, the game) after the edit was opened: nothing is written over it.
    SaveEdit stale(Only(user), folder);
    CHECK(stale.working().SetGold(0, 999));
    const Bytes other = MakeSave(8);
    WriteBytes(file, other);
    CHECK(stale.Apply() == SaveEdit::Result::kStale && ReadBytes(file) == other && ListBackups(folder).empty());
    WriteBytes(file, MakeSave(4));

    // Two copies of one save never write over each other.
    SaveEdit editor(Only(user), folder), other_copy(Only(user), folder);
    CHECK(editor.working().SetLevel(0, 0, 30) && other_copy.working().SetGold(0, 5));
    CHECK(other_copy.Apply() == SaveEdit::Result::kApplied);
    CHECK(editor.Apply() == SaveEdit::Result::kStale && SaveModel::FromFile(file).slots()[0].gold == 5);
    CHECK(SaveModel::DisplayedLevel(SaveModel::FromFile(file).Character(0, 0)->level_stored) == 15);

    // One model: the same edit through two SaveEdit instances produces the same bytes.
    const fs::path other_user = base / "edit_safety_b";
    WriteBytes(SaveFile(other_user, "CUSA10454", "SAVEDATA00"), MakeSave(4));
    SaveEdit b(Only(other_user), BackupFolder(other_user));
    WriteBytes(file, MakeSave(4));
    SaveEdit c(Only(user), folder);
    for (SaveEdit* edit : {&b, &c}) {
        CHECK(edit->working().SetGold(1, 321) && edit->working().SetLevel(0, 0, 40) && edit->working().SetSkillPoints(0, 0, 9) &&
              edit->working().SetHp(0, 0, 100, 200));
    }
    CHECK(b.working().Serialize() == c.working().Serialize());

    // An invalid appearance value stops the write.
    Bytes odd = MakeSave(4);
    odd[RecordOffset(0, 0) + kCharColor] = 9;
    Seal(odd);
    const fs::path odd_user = base / "edit_safety_c";
    WriteBytes(SaveFile(odd_user, "CUSA10454", "SAVEDATA00"), odd);
    SaveEdit invalid(Only(odd_user), BackupFolder(odd_user));
    CHECK(invalid.working().SetGold(0, 1) && invalid.Apply() == SaveEdit::Result::kInvalidAppearance);
    CHECK(ReadBytes(SaveFile(odd_user, "CUSA10454", "SAVEDATA00")) == odd);
}

// ---- PS3 saves: PARAM.PFD, decryption, plaintext and the import diagnosis ----

constexpr uint8_t kVectorPfd[672] = {
    0x00, 0x00, 0x00, 0x00, 0x50, 0x46, 0x44, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x50, 0x41, 0x52, 0x41, 0x4D, 0x2E, 0x53, 0x46,
    0x4F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0xB0,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x53, 0x41, 0x56, 0x45, 0x30, 0x2E, 0x44, 0x41,
    0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x74, 0x09, 0x5C, 0x22, 0xA7, 0x70, 0xD3, 0xED, 0x49, 0x88, 0x51, 0xDA, 0xFA, 0x1D, 0x89, 0x47,
    0x0D, 0xE7, 0xB1, 0x51, 0x7B, 0x43, 0x23, 0x13, 0x4C, 0x7E, 0x2B, 0x12, 0x8D, 0xC2, 0x57, 0xE2,
    0x8E, 0xEE, 0x64, 0x14, 0x23, 0xDA, 0xF7, 0x04, 0xAC, 0x23, 0x34, 0xB0, 0x4F, 0xDB, 0x90, 0x31,
    0xB1, 0xC8, 0xD4, 0x86, 0x0B, 0x17, 0xAB, 0xD5, 0xFD, 0xB9, 0xC8, 0x05, 0x85, 0x84, 0x2F, 0xE3,
    0xE0, 0x8E, 0x93, 0x28, 0xF9, 0xBB, 0x14, 0x19, 0x90, 0x04, 0xC3, 0xDC, 0x59, 0xB0, 0x8C, 0x27,
    0xC2, 0xE5, 0x6C, 0xE5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2D,
};
constexpr uint8_t kVectorCipher[48] = {
    0x35, 0xD9, 0xDC, 0xDB, 0x82, 0x9F, 0xEC, 0x33, 0x52, 0xE7, 0xBF, 0x10, 0xB8, 0x4B, 0xE4, 0xA5,
    0xFB, 0x7B, 0xB0, 0xB9, 0x3F, 0x46, 0xC5, 0xAC, 0x4E, 0x99, 0x43, 0x13, 0xE7, 0xDC, 0x61, 0x40,
    0xEE, 0x06, 0x75, 0x53, 0x9E, 0x6A, 0x35, 0xA3, 0x76, 0x43, 0xFB, 0xD3, 0x10, 0x42, 0x74, 0x9E,
};
constexpr uint8_t kVectorPlain[45] = {
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C,
};

Bytes FromHex(const char* hex) {
    Bytes bytes;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
        bytes.push_back(uint8_t(std::stoi(std::string(hex + i, 2), nullptr, 16)));
    }
    return bytes;
}

std::string ToHex(const uint8_t* data, size_t size) {
    std::string text;
    char buffer[3];
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(buffer, sizeof(buffer), "%02x", data[i]);
        text += buffer;
    }
    return text;
}

void PutBe64(Bytes& bytes, size_t at, uint64_t value) {
    for (int i = 0; i < 8; ++i) bytes[at + size_t(i)] = uint8_t(value >> (8 * (7 - i)));
}

// The PS3 container key and hash key, as observed on real saves; the tests build their own fixtures from them.
const Bytes kTestContainerKey = FromHex("D413B89663E1FE9F75143D3BB4565274");
const Bytes kTestHashKey = FromHex("100b0fa3430e545f0a12602538469ea0ba6e879f");

struct Ps3Fixture {
    Bytes pfd;
    Bytes cipher;
    Bytes plain;
};

// A PS3 save as the console stores it: SAVE0.DAT encrypted block by block, PARAM.PFD describing it.
Ps3Fixture MakePs3Fixture(const Bytes& plain, uint32_t seed = 1, uint64_t version = 4, const Bytes& hash_key = kTestHashKey) {
    Ps3Fixture fixture;
    fixture.plain = plain;
    const Bytes file_key = RandomBytes(16, seed);
    Bytes padded = plain;
    padded.resize((plain.size() + 15) / 16 * 16, 0);

    crypto::AesKey block;
    CHECK(block.Init(crypto::AesMode::kEcb, file_key.data()));
    Bytes mask(padded.size(), 0);
    for (size_t i = 0; i < padded.size() / 16; ++i) {
        for (int shift = 0; shift < 8; ++shift) mask[i * 16 + size_t(7 - shift)] = uint8_t(uint64_t(i) >> (8 * shift));
    }
    CHECK(block.Encrypt(mask.data(), mask.size()));
    fixture.cipher = padded;
    for (size_t i = 0; i < padded.size(); ++i) fixture.cipher[i] ^= mask[i];
    CHECK(block.Encrypt(fixture.cipher.data(), fixture.cipher.size()));

    Bytes entry_key = file_key;
    const Bytes rest = RandomBytes(48, seed + 100);
    entry_key.insert(entry_key.end(), rest.begin(), rest.end());
    crypto::AesKey container;
    uint8_t iv[16];
    std::memcpy(iv, hash_key.data(), 16);
    CHECK(container.Init(crypto::AesMode::kCbc, kTestContainerKey.data()));
    CHECK(container.Encrypt(entry_key.data(), entry_key.size(), iv));
    uint8_t mac[20];
    CHECK(crypto::HmacSha1(hash_key.data(), hash_key.size(), fixture.cipher.data(), fixture.cipher.size(), mac));

    const size_t slots = 57, capacity = 114, first = 0x78 + slots * 8;
    fixture.pfd.assign(first + capacity * 0x110 + 0x4A0, 0);
    PutBe64(fixture.pfd, 0, 0x50464442);
    PutBe64(fixture.pfd, 8, version);
    const Bytes header_iv = RandomBytes(16, seed + 7);
    std::memcpy(fixture.pfd.data() + 16, header_iv.data(), 16);
    PutBe64(fixture.pfd, 0x60, slots);
    PutBe64(fixture.pfd, 0x68, capacity);
    PutBe64(fixture.pfd, 0x70, 2);
    for (size_t i = 0; i < slots; ++i) PutBe64(fixture.pfd, 0x78 + i * 8, capacity);
    const auto put_entry = [&](size_t index, const char* name, uint64_t size, const uint8_t* key, const uint8_t* hash) {
        const size_t base = first + index * 0x110;
        PutBe64(fixture.pfd, base, capacity);
        std::memcpy(fixture.pfd.data() + base + 8, name, std::strlen(name));
        if (key) std::memcpy(fixture.pfd.data() + base + 0x50, key, 64);
        if (hash) std::memcpy(fixture.pfd.data() + base + 0x90, hash, 20);
        PutBe64(fixture.pfd, base + 0x108, size);
    };
    put_entry(0, "PARAM.SFO", 2736, nullptr, nullptr);
    put_entry(1, "SAVE0.DAT", plain.size(), entry_key.data(), mac);
    return fixture;
}

void PutBe32(Bytes& bytes, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + size_t(i)] = uint8_t(value >> (8 * (3 - i)));
}

void PutBe16(Bytes& bytes, size_t at, uint16_t value) {
    bytes[at] = uint8_t(value >> 8);
    bytes[at + 1] = uint8_t(value);
}

// A decrypted PS3 SAVE0.DAT: the Pro layout with big-endian values and a big-endian CRC-32.
Bytes MakePs3Plain(uint8_t seed = 0) {
    Bytes bytes(kSaveFileSize, 0xCC);
    std::memcpy(bytes.data() + kSaveMagicOffset, "DCSD", 4);
    const uint32_t header[] = {36, 490, 532, 0x56, 0x3B, 1, 3, 7, 8, 3, 0x45, 1, 1, 0x42, 0x90, 1, 1, 1};
    for (size_t i = 0; i < std::size(header); ++i) PutBe32(bytes, 8 + 4 * i, header[i]);
    const uint32_t gold[] = {1234567, 7757, 42};
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) PutBe32(bytes, slot * kDataSlotStride + kDataSlotGoldOffset, gold[slot]);
    const auto character = [&](int slot, int index, const char* name, uint8_t level, uint16_t skill_points, uint16_t hp) {
        const size_t base = RecordOffset(slot, index);
        PutBe32(bytes, base, 0);
        PutBe32(bytes, base + kCharHpCurrent, hp);
        PutBe32(bytes, base + kCharHpMax, hp);
        PutBe16(bytes, base + kCharSkillPoints, skill_points);
        bytes[base + kCharLevel] = level;
        std::memset(bytes.data() + base + kCharName, 0, kCharNameBytes);
        std::memcpy(bytes.data() + base + kCharName, name, std::strlen(name));
        PutBe32(bytes, base + 0x248 + 0x44, 100000);
        PutBe32(bytes, base + 0x248 + 0x48, 100000);
    };
    character(0, 0, "Eppie", 14, 15, 349);
    character(0, 1, "Allie", 0, 1, 300);
    character(2, 3, "Morgan", 98, 255, 1234);
    for (size_t i = 0x100; i < 0x100 + 64; ++i) bytes[i] = uint8_t(seed + i * 13);
    PutBe32(bytes, 0, SaveCrc32(bytes));
    return bytes;
}

void WritePs3SaveFolder(const fs::path& folder, const Ps3Fixture& fixture, const char* directory_title = "BLUS30767") {
    WriteBytes(folder / "SAVE0.DAT", fixture.cipher);
    WriteBytes(folder / "PARAM.PFD", fixture.pfd);
    WriteBytes(folder / "PARAM.SFO", SyntheticSfo({{"SAVEDATA_DIRECTORY", std::string(directory_title) + "-AUTO_0-"}}));
}

void TestPs3Crypto() {
    // RFC 2202, test cases 1 and 2.
    uint8_t mac[20];
    const Bytes key1(20, 0x0b);
    CHECK(crypto::HmacSha1(key1.data(), key1.size(), reinterpret_cast<const uint8_t*>("Hi There"), 8, mac));
    CHECK(ToHex(mac, 20) == "b617318655057264e28bc0b6fb378c8ef146be00");
    CHECK(crypto::HmacSha1(reinterpret_cast<const uint8_t*>("Jefe"), 4, reinterpret_cast<const uint8_t*>("what do ya want for nothing?"), 28, mac));
    CHECK(ToHex(mac, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    const Bytes key6(80, 0xaa); // a key longer than the block
    const char* text6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(crypto::HmacSha1(key6.data(), key6.size(), reinterpret_cast<const uint8_t*>(text6), std::strlen(text6), mac));
    CHECK(ToHex(mac, 20) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
    CHECK(crypto::HmacSha1(nullptr, 0, nullptr, 0, mac) && ToHex(mac, 20) == "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");

    // FIPS-197 appendix C.1.
    crypto::AesKey aes;
    const Bytes aes_key = FromHex("000102030405060708090a0b0c0d0e0f");
    Bytes block = FromHex("00112233445566778899aabbccddeeff");
    CHECK(aes.Init(crypto::AesMode::kEcb, aes_key.data()) && aes.Encrypt(block.data(), 16));
    CHECK(ToHex(block.data(), 16) == "69c4e0d86a7b0430d8cdb78070b4c55a");
    CHECK(aes.Decrypt(block.data(), 16) && ToHex(block.data(), 16) == "00112233445566778899aabbccddeeff");

    // A vector produced by a separate implementation: the whole unwrapping of a 45-byte file (padded to 48).
    const Bytes pfd(std::begin(kVectorPfd), std::end(kVectorPfd));
    const Bytes cipher(std::begin(kVectorCipher), std::end(kVectorCipher));
    Bytes plain;
    CHECK(DecryptPs3Save0(pfd, cipher, "BLUS30767-AUTO_0-", plain) == Ps3Status::kOk);
    CHECK(plain == Bytes(std::begin(kVectorPlain), std::end(kVectorPlain)));
    Ps3Entry entry;
    CHECK(FindPfdEntry(pfd, "SAVE0.DAT", entry) == Ps3Status::kOk && entry.size == 45);
}

void TestPfdParsing() {
    const Ps3Fixture good = MakePs3Fixture(MakePs3Plain());
    Ps3Pfd header;
    CHECK(ParsePfd(good.pfd, header) == Ps3Status::kOk && header.version == 4 && header.capacity == 114 && header.used == 2);
    Ps3Entry entry;
    CHECK(FindPfdEntry(good.pfd, "SAVE0.DAT", entry) == Ps3Status::kOk && entry.size == kSaveFileSize);
    CHECK(FindPfdEntry(good.pfd, "save0.dat", entry) == Ps3Status::kOk && entry.size == kSaveFileSize); // protected: listed
    CHECK(FindPfdEntry(good.pfd, "PARAM.SFO", entry) == Ps3Status::kOk && entry.size == 2736);
    CHECK(FindPfdEntry(good.pfd, "OTHER.DAT", entry) == Ps3Status::kNoEntry);
    CHECK(FindPfdEntry(good.pfd, "", entry) == Ps3Status::kNoEntry);
    CHECK(good.cipher.size() == kSaveFileSize + 8); // the logical size differs from the stored size only by alignment

    CHECK(ParsePfd(Bytes{}, header) == Ps3Status::kTruncated && ParsePfd(Bytes(0x77, 0), header) == Ps3Status::kTruncated);
    for (const size_t cut : {size_t(0x78), size_t(0x240), size_t(0x2000), good.pfd.size() - 0x4A0 - 1}) {
        const Bytes shorter(good.pfd.begin(), good.pfd.begin() + long(cut));
        CHECK(ParsePfd(shorter, header) == Ps3Status::kTruncated && FindPfdEntry(shorter, "SAVE0.DAT", entry) != Ps3Status::kOk);
    }
    Bytes wrong_magic = good.pfd;
    wrong_magic[7] = 'X';
    CHECK(ParsePfd(wrong_magic, header) == Ps3Status::kNotPfd);
    for (const uint64_t version : {uint64_t(0), uint64_t(1), uint64_t(3), uint64_t(5), ~uint64_t(0)}) {
        Bytes other = good.pfd;
        PutBe64(other, 8, version);
        CHECK(ParsePfd(other, header) == Ps3Status::kUnsupportedVersion);
    }

    // Counts and sizes that point outside the file or overflow.
    const struct {
        size_t at;
        uint64_t value;
    } malformed[] = {{0x60, 5000}, {0x60, ~uint64_t(0)}, {0x68, 5000}, {0x68, ~uint64_t(0)}, {0x70, 115}, {0x70, ~uint64_t(0)}};
    for (const auto& item : malformed) {
        Bytes other = good.pfd;
        PutBe64(other, item.at, item.value);
        CHECK(ParsePfd(other, header) == Ps3Status::kBadEntry);
    }
    Bytes beyond = good.pfd;
    PutBe64(beyond, 0x68, 4000);
    beyond.resize(0x1000);
    CHECK(ParsePfd(beyond, header) != Ps3Status::kOk);
    const size_t first = 0x78 + 57 * 8, save0 = first + 0x110;
    for (const uint64_t size : {uint64_t(0), uint64_t(65) << 20, ~uint64_t(0)}) {
        Bytes other = good.pfd;
        PutBe64(other, save0 + 0x108, size);
        CHECK(FindPfdEntry(other, "SAVE0.DAT", entry) == Ps3Status::kBadEntry);
    }
    Bytes no_terminator = good.pfd; // a name that fills its field without ending
    std::memset(no_terminator.data() + save0 + 8, 'A', 65);
    CHECK(FindPfdEntry(no_terminator, "SAVE0.DAT", entry) == Ps3Status::kNoEntry);
}

void TestPs3Decryption() {
    const Bytes plain = MakePs3Plain(3);
    const Ps3Fixture fixture = MakePs3Fixture(plain, 5);
    Bytes out;
    CHECK(DecryptPs3Save0(fixture.pfd, fixture.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kOk && out == plain);
    CHECK(out.size() == kSaveFileSize && fixture.cipher != plain);

    // Every Dragon's Crown save directory, in any case and with any suffix; nothing else.
    for (const char* title : {"BLUS30767", "BLES01950", "NPEB01836", "NPUB31235", "BLJM61041", "BCAS20298", "BCAS20290", "BCAS20288"}) {
        CHECK(IsDragonsCrownPs3Directory(title) && IsDragonsCrownPs3Directory(std::string(title) + "-AUTO_0-"));
        CHECK(DecryptPs3Save0(fixture.pfd, fixture.cipher, std::string(title) + "-AUTO_0-", out) == Ps3Status::kOk);
    }
    CHECK(IsDragonsCrownPs3Directory("blus30767-auto_0-"));
    for (const char* other : {"BLUS30768-AUTO_0-", "BLUS3076", "", "XBLUS30767", "NPUB30389", "CUSA10454", "PCSE00019"}) {
        CHECK(!IsDragonsCrownPs3Directory(other));
        CHECK(DecryptPs3Save0(fixture.pfd, fixture.cipher, other, out) == Ps3Status::kUnknownTitle && out.empty());
    }

    // Tampering: the stored hash covers the ciphertext, so any change is caught before any plaintext is produced.
    for (const size_t at : {size_t(0), size_t(16), fixture.cipher.size() / 2, fixture.cipher.size() - 1}) {
        Bytes bad = fixture.cipher;
        bad[at] ^= 0x01;
        CHECK(DecryptPs3Save0(fixture.pfd, bad, "BLUS30767-AUTO_0-", out) == Ps3Status::kHashMismatch && out.empty());
    }
    for (const size_t at : {size_t(0x78 + 57 * 8 + 0x110 + 0x90), size_t(0x78 + 57 * 8 + 0x110 + 0x93)}) {
        Bytes bad = fixture.pfd;
        bad[at] ^= 0xFF;
        CHECK(DecryptPs3Save0(bad, fixture.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kHashMismatch);
    }
    // A different secure ID gives a different hash key: the hash cannot match.
    Bytes other_key = kTestHashKey;
    other_key[19] ^= 0x01;
    const Ps3Fixture wrong = MakePs3Fixture(plain, 5, 4, other_key);
    CHECK(DecryptPs3Save0(wrong.pfd, wrong.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kHashMismatch);
    // A damaged entry key still passes the hash, and decrypts to something that is not a save: the plaintext check catches it.
    Bytes bad_key = fixture.pfd;
    bad_key[0x78 + 57 * 8 + 0x110 + 0x50] ^= 0x01;
    CHECK(DecryptPs3Save0(bad_key, fixture.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kOk && !InspectPs3Plaintext(out).valid);

    // Sizes: shorter, longer, or not what the entry records.
    for (const size_t size : {fixture.cipher.size() - 16, fixture.cipher.size() + 16, size_t(0), size_t(1)}) {
        Bytes resized = fixture.cipher;
        resized.resize(size);
        CHECK(DecryptPs3Save0(fixture.pfd, resized, "BLUS30767-AUTO_0-", out) == Ps3Status::kBadSize);
    }
    Bytes v3 = fixture.pfd;
    PutBe64(v3, 8, 3);
    CHECK(DecryptPs3Save0(v3, fixture.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kUnsupportedVersion);
    Bytes none = fixture.pfd;
    std::memset(none.data() + 0x78 + 57 * 8 + 0x110 + 8, 0, 9);
    CHECK(DecryptPs3Save0(none, fixture.cipher, "BLUS30767-AUTO_0-", out) == Ps3Status::kNoEntry);
    CHECK(std::string(Ps3StatusText(Ps3Status::kHashMismatch)).find("hash") != std::string::npos && *Ps3StatusText(Ps3Status::kOk) == '\0');
}

void TestPs3Plaintext() {
    const Bytes plain = MakePs3Plain();
    const Ps3Plaintext info = InspectPs3Plaintext(plain);
    CHECK(info.valid && info.slots == 3 && info.characters == 3);
    CHECK(info.gold[0] == 1234567 && info.gold[1] == 7757 && info.gold[2] == 42); // big-endian values
    CHECK(!ValidateSave(plain, *std::make_unique<std::vector<std::string>>())); // not a Pro save: its CRC is stored big-endian

    Bytes bad_crc = plain;
    bad_crc[0x500] ^= 1;
    CHECK(!InspectPs3Plaintext(bad_crc).valid);
    Bytes bad_magic = plain;
    bad_magic[4] = 'X';
    PutBe32(bad_magic, 0, SaveCrc32(bad_magic));
    CHECK(!InspectPs3Plaintext(bad_magic).valid);
    Bytes little = plain;
    for (int i = 0; i < 4; ++i) little[size_t(i)] = plain[size_t(3 - i)];
    CHECK(!InspectPs3Plaintext(little).valid); // a little-endian CRC is the Pro's, not the PS3's
    CHECK(!InspectPs3Plaintext(Bytes(plain.begin(), plain.end() - 1)).valid && !InspectPs3Plaintext({}).valid);
    CHECK(!InspectPs3Plaintext(Bytes(kSaveFileSize + 8, 0)).valid);
    CHECK(!InspectPs3Plaintext(MakeSave()).valid); // a real Pro layout is not read as PS3
}

Bytes ConvertedPs3(const Bytes& plain) {
    Bytes pro;
    Ps3ConvertReport report;
    ConvertPs3ToPro(plain, pro, report);
    return pro;
}

void TestPs3Import(const fs::path& base) {
    const fs::path user = base / "ps3_user";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave(1));
    const DiscoveredSave destination = Only(user);
    const fs::path root = base / "ps3_sources";
    const Ps3Fixture fixture = MakePs3Fixture(MakePs3Plain(), 9);
    WritePs3SaveFolder(root / "set" / "BLUS30767-AUTO_0-", fixture);

    // The whole chain runs in memory: PFD, hash, decryption, plaintext check, conversion and the Pro checks.
    const Bytes converted = ConvertedPs3(MakePs3Plain());
    const std::string cipher_before = Hash(root / "set" / "BLUS30767-AUTO_0-" / "SAVE0.DAT");
    const std::string pfd_before = Hash(root / "set" / "BLUS30767-AUTO_0-" / "PARAM.PFD");
    const auto times_before = fs::last_write_time(root / "set" / "BLUS30767-AUTO_0-" / "SAVE0.DAT");
    const ImportResult result = InspectImport(root / "set");
    CHECK(result.valid && result.block == ImportBlock::kNone && result.source == ImportSource::kFolder);
    CHECK(result.platform == "PS3" && result.region == "USA" && result.title_id == "BLUS30767");
    CHECK(result.encrypted && result.pfd_state == "Valid" && result.decryption == "Supported" && result.conversion_required);
    CHECK(result.data_slots == 3 && result.characters == 3 && result.has_param_pfd && result.has_param_sfo);
    CHECK(result.format == "PS3 layout, big-endian (decrypted)" && result.bytes == converted && result.detected == "PS3 Dragon's Crown (USA)");
    std::vector<std::string> problems;
    CHECK(ValidateSave(result.bytes, problems) && std::memcmp(result.bytes.data() + 4, "DCSD", 4) == 0);
    CHECK(Hash(root / "set" / "BLUS30767-AUTO_0-" / "SAVE0.DAT") == cipher_before && Hash(root / "set" / "BLUS30767-AUTO_0-" / "PARAM.PFD") == pfd_before);
    CHECK(fs::last_write_time(root / "set" / "BLUS30767-AUTO_0-" / "SAVE0.DAT") == times_before);

    // Importing writes the converted Pro save, after a backup of the save it replaces, and nothing else.
    const ImportPlan plan = PlanImport(root / "set", destination, ImportMode::kPs3);
    CHECK(plan.refused.empty() && !plan.unverified_platform && plan.destination == file);
    const SaveOpResult imported = CommitImportPlan(plan, destination, BackupFolder(user));
    CHECK(imported.ok && ReadBytes(file) == converted && !fs::exists(file.string() + ".tmp"));
    CHECK(ListBackups(BackupFolder(user)).size() == 1 && ReadBytes(ListBackups(BackupFolder(user))[0].file) == MakeSave(1));
    CHECK(Hash(root / "set" / "BLUS30767-AUTO_0-" / "SAVE0.DAT") == cipher_before && Hash(root / "set" / "BLUS30767-AUTO_0-" / "PARAM.PFD") == pfd_before);

    // A destination that changed after the page loaded it is not overwritten.
    WriteBytes(file, MakeSave(1));
    const DiscoveredSave loaded = Only(user);
    const Bytes newer = MakeSave(9);
    WriteBytes(file, newer);
    const SaveOpResult stale = CommitImportPlan(PlanImport(root / "set", loaded, ImportMode::kPs3), loaded, BackupFolder(user));
    CHECK(!stale.ok && stale.stale && ReadBytes(file) == newer && ListBackups(BackupFolder(user)).size() == 1);
    WriteBytes(file, MakeSave(1));

    // A layout version other than 36 is refused with its own reason, and nothing of it is kept.
    Bytes other_version = MakePs3Plain();
    PutBe32(other_version, 8, 38);
    PutBe32(other_version, 0, SaveCrc32(other_version));
    WritePs3SaveFolder(root / "v38" / "BLUS30767-AUTO_0-", MakePs3Fixture(other_version, 9));
    const ImportResult unsupported = InspectImport(root / "v38");
    CHECK(!unsupported.valid && unsupported.block == ImportBlock::kPs3UnsupportedLayout && unsupported.bytes.empty());
    CHECK(unsupported.characters == 3 && unsupported.decryption == "Supported" && !PlanImport(root / "v38", destination, ImportMode::kPs3).refused.empty());
    CHECK(!CommitImportPlan(PlanImport(root / "v38", destination, ImportMode::kPs3), destination, BackupFolder(user)).ok && ReadBytes(file) == MakeSave(1));

    // The same through a ZIP of the save folder.
    const Bytes zip = BuildZip({{"BLUS30767-AUTO_0-/SAVE0.DAT", fixture.cipher, 8},
                                {"BLUS30767-AUTO_0-/PARAM.PFD", fixture.pfd, 8},
                                {"BLUS30767-AUTO_0-/PARAM.SFO", SyntheticSfo({{"SAVEDATA_DIRECTORY", "BLUS30767-AUTO_0-"}}), 0}});
    const ImportResult zipped = InspectZip(zip, "ps3.zip");
    CHECK(zipped.valid && zipped.block == ImportBlock::kNone && zipped.encrypted && zipped.pfd_state == "Valid" && zipped.decryption == "Supported");
    CHECK(zipped.data_slots == 3 && zipped.characters == 3 && zipped.entry == "BLUS30767-AUTO_0-/SAVE0.DAT");
    // The save directory is the entry's folder, whatever the ZIP's outer folders are called.
    const Bytes nested = BuildZip({{"Set 4 - x/BLJM61041-AUTO_0-/SAVE0.DAT", fixture.cipher, 0}, {"Set 4 - x/BLJM61041-AUTO_0-/PARAM.PFD", fixture.pfd, 0}});
    const ImportResult nested_result = InspectZip(nested, "set.zip");
    CHECK(nested_result.valid && nested_result.region == "Japan" && nested_result.pfd_state == "Valid" && nested_result.bytes == converted);
    const Bytes missing_pfd = BuildZip({{"BLUS30767-AUTO_0-/SAVE0.DAT", fixture.cipher, 0}});
    const ImportResult no_pfd = InspectZip(missing_pfd, "ps3.zip");
    CHECK(no_pfd.block == ImportBlock::kPs3NeedsPfd && no_pfd.pfd_state == "Missing" && !no_pfd.valid);
    CHECK(std::string(ImportBlockText(no_pfd.block)).find("PARAM.PFD") != std::string::npos);
    CHECK(InspectZip(BuildZip({{"a/SAVE0.DAT", fixture.cipher, 0}, {"b/SAVE0.DAT", MakePs3Fixture(MakePs3Plain(5), 4).cipher, 0}}), "x.zip").block ==
          ImportBlock::kAmbiguous);
    ZipLimits cap;
    cap.max_total = 1000;
    CHECK(InspectZip(zip, "ps3.zip", cap).block == ImportBlock::kArchiveLimits);
    CHECK(InspectZip(BuildZip({{"../BLUS30767-AUTO_0-/SAVE0.DAT", fixture.cipher, 0}}), "x.zip").block == ImportBlock::kArchiveUnsafe);

    // A raw encrypted SAVE0.DAT says what is missing; each defect in a complete save has its own reason.
    WriteBytes(root / "raw" / "SAVE0.DAT", fixture.cipher);
    const ImportResult raw = InspectImport(root / "raw" / "SAVE0.DAT");
    CHECK(raw.block == ImportBlock::kPs3NeedsPfd && raw.encrypted && !raw.has_param_pfd);
    Ps3Fixture tampered = fixture;
    tampered.cipher[100] ^= 1;
    WritePs3SaveFolder(root / "tampered" / "BLUS30767-AUTO_0-", tampered);
    CHECK(InspectImport(root / "tampered").block == ImportBlock::kPs3HashMismatch);
    WritePs3SaveFolder(root / "other_game" / "BLUS30768-AUTO_0-", fixture, "BLUS30768");
    const ImportResult other_game = InspectImport(root / "other_game");
    CHECK(other_game.block == ImportBlock::kPs3PfdInvalid || other_game.block == ImportBlock::kPs3UnknownTitle);
    WritePs3SaveFolder(root / "ours" / "NPUB31235-AUTO_0-", fixture, "NPUB31235");
    CHECK(InspectImport(root / "ours").valid && InspectImport(root / "ours").region == "USA");
    Ps3Fixture short_pfd = fixture;
    short_pfd.pfd.resize(0x240);
    WritePs3SaveFolder(root / "short" / "BLUS30767-AUTO_0-", short_pfd);
    const ImportResult short_result = InspectImport(root / "short");
    CHECK(short_result.block == ImportBlock::kPs3PfdInvalid && short_result.pfd_state == "Invalid" && !short_result.errors.empty());
    const Ps3Fixture version3 = MakePs3Fixture(MakePs3Plain(), 9, 3);
    WritePs3SaveFolder(root / "v3" / "BLUS30767-AUTO_0-", version3);
    CHECK(InspectImport(root / "v3").block == ImportBlock::kPs3PfdInvalid);
    Ps3Fixture broken_key = fixture;
    broken_key.pfd[0x78 + 57 * 8 + 0x110 + 0x50] ^= 1;
    WritePs3SaveFolder(root / "broken" / "BLUS30767-AUTO_0-", broken_key);
    const ImportResult broken = InspectImport(root / "broken");
    CHECK(broken.block == ImportBlock::kPs3Undecryptable && !broken.errors.empty());
    CHECK(PlanImport(root / "broken", destination, ImportMode::kPs3).refused.find("decrypted") != std::string::npos);

    // A save somebody already decrypted is converted too.
    WriteBytes(root / "plain" / "SAVE0.DAT", MakePs3Plain());
    const ImportResult plain = InspectImport(root / "plain" / "SAVE0.DAT");
    CHECK(plain.valid && !plain.encrypted && plain.conversion_required && plain.characters == 3 && plain.bytes == converted);
    CHECK(plain.format == "PS3 layout, big-endian");

    // The conversion lives in one place, and no plaintext or converted save reaches the disk except through the import commit.
    for (const char* name : {"launcher/save_ps3.cpp", "launcher/save_page_model.cpp", "launcher/save_page.cpp"}) {
        CHECK(Source(name).find("ConvertPs3ToPro") == std::string::npos);
    }
    const std::string describe = Body(Source("launcher/save_import.cpp"), "void DescribePs3(");
    CHECK(describe.find("ConvertPs3ToPro") != std::string::npos && describe.find("ofstream") == std::string::npos &&
          describe.find("WriteSaveAtomically") == std::string::npos && describe.find("ValidateSave(pro") != std::string::npos);
    CHECK(Source("launcher/save_ps3_layout.cpp").find("ofstream") == std::string::npos && Source("launcher/save_ps3_layout.cpp").find("filesystem") == std::string::npos);
    CHECK(ReadBytes(file) == MakeSave(1));
}

// The Saves page manages saves; characters are edited on the Editor page.
void TestSavePageUx() {
    const std::string page = Source("launcher/save_page.cpp");
    const std::string header = Source("launcher/save_page.h");

    for (const char* gone : {"Edit save", "DrawEdit", "DrawEditActions", "ApplyEdit", "SaveEdit", "edit_", "InputInt", "NumberField", "SetGold(",
                             "SetLevel(", "SetSkillPoints(", "SetHp(", "kGoldRange", "kLevelRange", "kSkillPointRange", "kHpRange", "Apply Changes",
                             "Discard Changes", "Apply changes", "Discard changes", "\"Skill Points\"", "\"Current HP\"", "\"Max HP\"", "\"Level\"",
                             "\"Character\"", "\"DATA slot\"", "Unsaved changes", "bool dirty", "DirtyReason"}) {
        CHECK(page.find(gone) == std::string::npos);
        CHECK(header.find(gone) == std::string::npos);
    }
    CHECK(page.find("Tr(\"Gold\")") != std::string::npos); // the read-only slot summary only

    const std::string slots = Body(page, "void SavePage::DrawSlots(");
    CHECK(slots.find("ImGui::Input") == std::string::npos && slots.find("BeginCombo") == std::string::npos && slots.find("Drag") == std::string::npos);
    CHECK(slots.find("\"Open Editor\"") != std::string::npos && slots.find("hooks_.open_editor()") != std::string::npos);

    const std::string draw = Body(page, "void SavePage::Draw(trg::Ui& ui) {");
    CHECK(draw.find("DrawOverview(ui, *save);") < draw.find("DrawSlots(ui, *save);"));
    CHECK(draw.find("DrawSlots(ui, *save);") < draw.find("DrawManagement(ui, save);"));
    CHECK(draw.find("DrawManagement(ui, save);") < draw.find("DrawBackups(ui, *save);"));

    const std::string management = Body(page, "void SavePage::DrawManagement(");
    size_t last = 0;
    for (const char* action : {"\"Create backup\"", "\"Export PS4 save...\"", "\"Import PS4 save...\"", "\"Import PS3 save...\"", "\"Open backup folder\""}) {
        const size_t at = management.find(action, last);
        CHECK(at != std::string::npos && at >= last);
        last = at;
    }
    CHECK(management.find("\"Import save...\"") == std::string::npos && management.find("\"Export save...\"") == std::string::npos);
    CHECK(management.find("DrawImportMenu(ui, import_ps4, ImportMode::kPs4);") != std::string::npos);
    CHECK(management.find("DrawImportMenu(ui, import_ps3, ImportMode::kPs3);") != std::string::npos);
    CHECK(management.find("SavePick::kExportFolder") != std::string::npos);

    const std::string menu = Body(page, "void SavePage::DrawImportMenu(");
    for (const char* pick : {"kImportPs4File", "kImportPs4Folder", "kImportPs3File", "kImportPs3Folder"}) CHECK(menu.find(pick) != std::string::npos);
    CHECK(menu.find("ZIP archive of a PS3 save...") != std::string::npos && menu.find("PS3 save folder (SAVE0.DAT and PARAM.PFD)...") != std::string::npos);
    const std::string confirm = Body(page, "void SavePage::DrawImportConfirm(");
    CHECK(confirm.find("ui.Heading(ps3 ? \"Import PS3 save\" : \"Import PS4 save\");") != std::string::npos);
    for (const char* hidden : {"\"Platform\"", "\"Region\"", "\"Title ID\"", "\"Encrypted save\"", "\"PARAM.PFD\"", "\"PS3 decryption\"",
                               "\"Conversion\"", "\"Source layout\"", "\"SAVE0.DAT\"", "\"Backup\"", "Made first", "\"-\"",
                               "Nothing is written until you confirm"}) {
        CHECK(confirm.find(hidden) == std::string::npos);
    }
    for (const char* shown : {"{\"Source\", source}", "\"Source type\"", "{\"Save status\", Tr(\"Supported\")}", "\"DATA slots\"", "\"Characters\"",
                              "\"Destination\"", "Your current save will be backed up before it is replaced"}) {
        CHECK(confirm.find(shown) != std::string::npos);
    }
    CHECK(confirm.find("if (!blocked) rows.push_back({\"Save status\"") != std::string::npos);
    const std::string source_name = Body(page, "std::string ImportSourceName(");
    CHECK(source_name.find("kZip") != std::string::npos && source_name.find("parent_path().filename()") != std::string::npos &&
          source_name.find("absolute") == std::string::npos);
    CHECK(confirm.find("if (blocked) {") < confirm.find("\"Import and replace...\""));

    // The outline is the rect of one group around the whole preview, taken after the row table is closed; the last item's
    // rect or the cursor inside a row would give only the right-hand column.
    const size_t outline_begin = page.find("class PreviewOutline");
    const std::string outline = page.substr(outline_begin, page.find("SavePage::SavePage(", outline_begin) - outline_begin);
    CHECK(outline.find("ui.EndRows();") < outline.find("ImGui::BeginGroup();") && outline.find("ImGui::EndGroup();") < outline.find("GetItemRectMin()"));
    CHECK(outline.find("GetCursorScreenPos") == std::string::npos && outline.find("GetContentRegionAvail") == std::string::npos &&
          outline.find("GetCursorPos") == std::string::npos);
    CHECK(confirm.find("const PreviewOutline outline(ui, pending_import_);") < confirm.find("ui.Heading(ps3"));

    // One import engine: both pickers end in the same inspection and commit.
    CHECK(page.find("PlanImport(") != std::string::npos && page.find("CommitImportPlan(") != std::string::npos);
    CHECK(Source("launcher/save_page_model.cpp").find("InspectImport(source)") != std::string::npos);

    const std::string launcher = Source("launcher/launcher.cpp");
    CHECK(launcher.find("save_page_->dirty()") == std::string::npos && launcher.find("save_page_->DirtyReason()") == std::string::npos);
    const std::string choose = Body(launcher, "    void ChooseForSavePage(SavePick pick) {");
    for (const char* pick : {"kImportPs4File", "kImportPs3File", "kImportPs3Folder", "kImportPs4Folder", "kExportFolder"}) CHECK(choose.find(pick) != std::string::npos);
    CHECK(choose.find("PS3 save archive") == std::string::npos || choose.find("\"Save archive (ZIP, RAR)\"") != std::string::npos);

    // The Editor keeps editing the characters.
    const std::string editor = Source("launcher/editor_page.cpp");
    for (const char* setter : {"SetLevel(", "SetSkillPoints(", "SetHp("}) CHECK(editor.find(setter) != std::string::npos);
}

// Each import entry point takes only its own kind of source and says which one to use otherwise.
void TestImportModes(const fs::path& base) {
    const fs::path user = base / "modes_user";
    const fs::path file = SaveFile(user, "CUSA10454", "SAVEDATA00");
    WriteBytes(file, MakeSave(1));
    const DiscoveredSave destination = Only(user);
    const fs::path root = base / "modes_sources";
    const Bytes ps3_plain = MakePs3Plain();
    const Ps3Fixture fixture = MakePs3Fixture(ps3_plain, 9);
    WritePs3SaveFolder(root / "ps3_folder" / "BLUS30767-AUTO_0-", fixture);
    const Bytes ps3_zip = BuildZip({{"BLUS30767-AUTO_0-/SAVE0.DAT", fixture.cipher, 8},
                                    {"BLUS30767-AUTO_0-/PARAM.PFD", fixture.pfd, 8},
                                    {"BLUS30767-AUTO_0-/PARAM.SFO", SyntheticSfo({{"SAVEDATA_DIRECTORY", "BLUS30767-AUTO_0-"}}), 0}});
    WriteBytes(root / "ps3.zip", ps3_zip);
    const Bytes pro = MakeSave(5);
    WriteBytes(root / "pro_file" / "SAVE0.DAT", pro);
    WriteBytes(root / "pro_folder" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT", pro);
    WriteBytes(root / "pro.zip", BuildZip({{"CUSA10454/SAVEDATA00/SAVE0.DAT", pro, 8}}));
    WriteBytes(root / "ps3_raw" / "SAVE0.DAT", fixture.cipher);
    Bytes other_version = ps3_plain;
    PutBe32(other_version, 8, 38);
    PutBe32(other_version, 0, SaveCrc32(other_version));
    WritePs3SaveFolder(root / "ps3_v38" / "BLUS30767-AUTO_0-", MakePs3Fixture(other_version, 9));
    WriteBytes(root / "archive.rar", Bytes{'R', 'a', 'r', '!', 0x1A, 0x07, 0x00, 0x00});

    const std::string use_ps3 = "This is a PS3 save. Use Import PS3 save.";
    const std::string use_ps4 = "This is a Dragon's Crown Pro save. Use Import PS4 save.";

    // PS4 mode: native sources are accepted, any PS3 source is refused with guidance and never converted.
    for (const fs::path& source : {root / "pro_file" / "SAVE0.DAT", root / "pro_folder", root / "pro.zip"}) {
        const ImportPlan plan = PlanImport(source, destination, ImportMode::kPs4);
        CHECK(plan.refused.empty() && plan.mode == ImportMode::kPs4 && plan.inspected.valid && plan.inspected.bytes == pro);
        CHECK(!plan.inspected.encrypted && !plan.inspected.conversion_required && plan.inspected.ps3_layout_version == 0);
    }
    const ImportPlan folder_plan = PlanImport(root / "pro_folder", destination, ImportMode::kPs4);
    CHECK(folder_plan.inspected.platform == "PS4" && folder_plan.inspected.title_id == "CUSA10454" && folder_plan.inspected.region == "USA");
    CHECK(folder_plan.inspected.data_slots == 3 && folder_plan.destination_identity == "CUSA10454 / SAVEDATA00 (USA)");
    for (const fs::path& source : {root / "ps3_folder", root / "ps3.zip", root / "ps3_raw" / "SAVE0.DAT", root / "ps3_v38"}) {
        const ImportPlan plan = PlanImport(source, destination, ImportMode::kPs4);
        CHECK(plan.refused == use_ps3 && !CommitImportPlan(plan, destination, BackupFolder(user)).ok);
    }

    // PS3 mode: a supported version-36 folder and ZIP are accepted; a native save is refused with guidance.
    const Bytes converted = ConvertedPs3(ps3_plain);
    for (const fs::path& source : {root / "ps3_folder", root / "ps3.zip"}) {
        const ImportPlan plan = PlanImport(source, destination, ImportMode::kPs3);
        CHECK(plan.refused.empty() && plan.mode == ImportMode::kPs3 && plan.inspected.valid && plan.inspected.bytes == converted);
        CHECK(plan.inspected.platform == "PS3" && plan.inspected.region == "USA" && plan.inspected.title_id == "BLUS30767");
        CHECK(plan.inspected.encrypted && plan.inspected.pfd_state == "Valid" && plan.inspected.decryption == "Supported");
        CHECK(plan.inspected.ps3_layout_version == 36 && plan.inspected.data_slots == 3 && plan.inspected.characters == 3);
    }
    for (const fs::path& source : {root / "pro_file" / "SAVE0.DAT", root / "pro_folder", root / "pro.zip"}) {
        const ImportPlan plan = PlanImport(source, destination, ImportMode::kPs3);
        CHECK(plan.refused == use_ps4 && !CommitImportPlan(plan, destination, BackupFolder(user)).ok);
    }
    CHECK(ReadBytes(file) == MakeSave(1) && CountFiles(BackupFolder(user)) == 0);

    // PS3 mode keeps the precise reasons for what it cannot read.
    const ImportPlan raw = PlanImport(root / "ps3_raw" / "SAVE0.DAT", destination, ImportMode::kPs3);
    CHECK(raw.inspected.block == ImportBlock::kPs3NeedsPfd && raw.refused == ImportBlockText(ImportBlock::kPs3NeedsPfd));
    const ImportPlan v38 = PlanImport(root / "ps3_v38", destination, ImportMode::kPs3);
    CHECK(v38.inspected.block == ImportBlock::kPs3UnsupportedLayout && v38.inspected.ps3_layout_version == 38 && v38.inspected.bytes.empty());
    CHECK(v38.refused == ImportBlockText(ImportBlock::kPs3UnsupportedLayout));
    for (const ImportMode mode : {ImportMode::kPs4, ImportMode::kPs3}) {
        const ImportPlan rar = PlanImport(root / "archive.rar", destination, mode);
        CHECK(rar.inspected.block == ImportBlock::kRarArchive && rar.refused.find("Extract it first") != std::string::npos);
        CHECK(!PlanImport(root / "nothing_here", destination, mode).refused.empty());
    }

    // The shared commit still backs up first, writes atomically and refuses a destination that changed.
    const ImportPlan plan = PlanImport(root / "ps3.zip", destination, ImportMode::kPs3);
    const SaveOpResult imported = CommitImportPlan(plan, destination, BackupFolder(user));
    CHECK(imported.ok && ReadBytes(file) == converted && !fs::exists(file.string() + ".tmp"));
    CHECK(ListBackups(BackupFolder(user)).size() == 1 && ReadBytes(ListBackups(BackupFolder(user))[0].file) == MakeSave(1));
    WriteBytes(file, MakeSave(1));
    const DiscoveredSave loaded = Only(user);
    WriteBytes(file, MakeSave(9));
    const SaveOpResult stale = CommitImportPlan(PlanImport(root / "ps3_folder", loaded, ImportMode::kPs3), loaded, BackupFolder(user));
    CHECK(!stale.ok && stale.stale && ReadBytes(file) == MakeSave(9) && ListBackups(BackupFolder(user)).size() == 1);

    // Both ends of the guidance exist in Portuguese.
    const std::string portuguese = Source("launcher/localization/pt_br_ui_text.cpp");
    for (const char* text : {"Export PS4 save", "Export PS4 save...", "Import PS4 save", "Import PS4 save...", "Import PS3 save", "Import PS3 save...",
                             "This is a PS3 save. Use Import PS3 save.", "This is a Dragon's Crown Pro save. Use Import PS4 save.",
                             "This is a PS Vita save, not a PS3 save. Use Import PS4 save.", "Save status", "Your current save will be backed up before it is replaced", "Open Editor"}) {
        CHECK(portuguese.find(std::string("\n") + text + "\t") != std::string::npos);
    }
}

// ---- PS3 layout and conversion ----

uint32_t ReadLe32(const Bytes& bytes, size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
}

uint32_t ReadBe32(const Bytes& bytes, size_t at) {
    return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at + 1]) << 16 | uint32_t(bytes[at + 2]) << 8 | bytes[at + 3];
}

uint16_t ReadBe16(const Bytes& bytes, size_t at) { return uint16_t(bytes[at] << 8 | bytes[at + 1]); }

constexpr size_t kSlotBytes = 0xAF924;
constexpr size_t kItemSize = 0x54;
constexpr size_t kInventoryBase = 0xC0;
constexpr size_t kInventoryCount = 500;
constexpr size_t kEquipmentRelative = 0x248;
constexpr size_t kEquipmentCount = 10;

void TestPs3Layout() {
    const std::span<const Ps3LayoutNode> program = Ps3SlotProgram();
    CHECK(program.size() == 127);
    for (const Ps3LayoutNode& node : program) {
        if (node.op == Ps3Op::kRepeat) CHECK(node.value >= 2 && node.body >= 1);
        else CHECK(node.value < 4);
    }

    std::map<size_t, Ps3Kind> fields;
    size_t previous_end = 0x50, typed = 0, containers = 0;
    bool ordered = true;
    ForEachPs3SlotField([&](size_t offset, Ps3Kind kind) {
        ordered = ordered && offset >= previous_end;
        previous_end = offset + Ps3KindWidth(kind);
        typed += Ps3KindWidth(kind);
        containers += kind == Ps3Kind::kItemBits;
        fields[offset] = kind;
    });
    CHECK(ordered && previous_end == kSlotBytes);
    CHECK(fields.size() == 264641 && typed == 597709 && containers == 7540);

    // Anchors the Character Editor and the Saves page already depend on, at the widths the game code gives them.
    const auto kind_at = [&](size_t offset) { return fields.count(offset) ? int(fields[offset]) : -1; };
    CHECK(kind_at(kDataSlotGoldOffset) == int(Ps3Kind::kU32) && kind_at(0xAF284) == int(Ps3Kind::kU32));
    CHECK(kind_at(kCharacterOffset + kCharHpCurrent) == int(Ps3Kind::kU32) && kind_at(kCharacterOffset + kCharHpMax) == int(Ps3Kind::kU32));
    CHECK(kind_at(kCharacterOffset + kCharSkillPoints) == int(Ps3Kind::kU16));
    for (size_t i = 0; i < 3; ++i) CHECK(kind_at(kCharacterOffset + kCharLevel + i) == int(Ps3Kind::kByte));
    for (size_t i = 0; i < kCharNameBytes; ++i) CHECK(kind_at(kCharacterOffset + kCharName + i) == int(Ps3Kind::kByte));
    CHECK(kind_at(kCharacterOffset + kEquipmentRelative + 0x44) == int(Ps3Kind::kU32));
    CHECK(kind_at(kInventoryBase + 0x40) == int(Ps3Kind::kItemBits) && kind_at(kInventoryBase) == int(Ps3Kind::kU16));
    CHECK(kind_at(kInventoryBase + kInventoryCount * kItemSize - kItemSize + 0x40) == int(Ps3Kind::kItemBits));
}

// A PS3 plaintext with the fields the tests look at; everything else is the 0xCC fill the game leaves in padding.
void PutBeItem(Bytes& bytes, size_t item, uint16_t id, uint32_t bits, uint32_t durability) {
    PutBe16(bytes, item, id);
    PutBe32(bytes, item + 4, 0x41F00000);
    for (size_t effect = 1; effect < 8; ++effect) PutBe16(bytes, item + effect * 8, 0xFFFF);
    PutBe32(bytes, item + 0x40, bits);
    PutBe32(bytes, item + 0x44, durability);
    PutBe32(bytes, item + 0x48, durability);
    bytes[item + 0x4F] = 0xFF;
    bytes[item + 0x53] = 0x07;
}

Bytes MakeConvertiblePs3(uint8_t seed = 0) {
    Bytes bytes = MakePs3Plain(seed);
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        const size_t base = slot * kDataSlotStride;
        PutBeItem(bytes, base + kInventoryBase, 0x0185, 0x0000380C, 80);
        PutBeItem(bytes, base + kInventoryBase + kItemSize, 0xFFFF, 0x00000004, 0);
        PutBe32(bytes, base + 0xAF284, 4321);
    }
    for (size_t i = 0; i < 59; ++i) bytes[0x20EACC + i] = uint8_t(i % 3);
    PutBe32(bytes, 0x20EACC + 0x44, 1260);
    bytes[0x20EACC + 0x48] = 1;
    PutBe32(bytes, 0, SaveCrc32(bytes));
    return bytes;
}

Bytes Reseal(Bytes bytes) {
    PutBe32(bytes, 0, SaveCrc32(bytes));
    return bytes;
}

void TestPs3Conversion() {
    const Bytes plain = MakeConvertiblePs3(3);
    Bytes pro;
    Ps3ConvertReport report;
    CHECK(ConvertPs3ToPro(plain, pro, report) && report.converted && pro.size() == kSaveFileSize && report.source_version == 36);
    CHECK(report.converted_fields == 3 * 264641 + 59 + 6 + 1 + 2);
    CHECK(report.padding_bytes + 3 * 597709 + 59 + 6 + 4 + 2 + 0x50 == kSaveFileSize);

    // The Pro file as the Pro game would write it: its own header, little-endian fields, the Pro checksum.
    std::vector<std::string> problems;
    CHECK(ValidateSave(pro, problems) && std::memcmp(pro.data() + 4, "DCSD", 4) == 0 && ReadLe32(pro, 0) == SaveCrc32(pro));
    CHECK(ReadLe32(pro, 8) == 38 && ReadLe32(pro, 12) == 660 && ReadLe32(pro, 16) == 979 && ReadLe32(pro, 0x4C) == 1);
    const SaveModel model = SaveModel::FromBytes(pro);
    CHECK(model.valid() && model.slots()[0].gold == 1234567 && model.slots()[1].gold == 7757 && model.slots()[2].gold == 42);
    const CharacterView* first = model.Character(0, 0);
    CHECK(first && first->present && first->name == "Eppie" && SaveModel::DisplayedLevel(first->level_stored) == 15 &&
          first->skill_points == 15 && first->hp_current == 349 && first->hp_max == 349);
    const CharacterView* last = model.Character(2, 3);
    CHECK(last && last->name == "Morgan" && last->skill_points == 255 && last->hp_current == 1234);
    CHECK(model.Character(0, 1)->name == "Allie" && !model.Character(0, 2)->present && !model.Character(1, 0)->present);
    CHECK(ReadLe32(pro, RecordOffset(0, 0) + kEquipmentRelative + 0x44) == 100000 && ReadLe32(pro, RecordOffset(0, 0) + kEquipmentRelative + 0x48) == 100000);

    // Items: id and effect fields are reversed, and the packed word is re-packed, not just reversed.
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        const size_t base = slot * kDataSlotStride;
        CHECK(pro[base + kInventoryBase] == 0x85 && pro[base + kInventoryBase + 1] == 0x01);
        CHECK(ReadLe32(pro, base + kInventoryBase + 4) == 0x41F00000);
        CHECK(ReadLe32(pro, base + kInventoryBase + 0x40) == 0x22000070);
        CHECK(ReadLe32(pro, base + kInventoryBase + kItemSize + 0x40) == 0x20000000);
        CHECK(ReadLe32(pro, base + kInventoryBase + 0x44) == 80 && pro[base + kInventoryBase + 0x53] == 7);
        CHECK(ReadLe32(pro, base + 0xAF284) == 4321);
    }

    // Trailer fields and the root header.
    CHECK(pro[0x20EACC + 1] == 1 && pro[0x20EACC + 2] == 2 && ReadLe32(pro, 0x20EACC + 0x44) == 1260 && pro[0x20EACC + 0x48] == 1);

    // Padding is zero whatever the source held there, and changes nowhere else.
    CHECK(plain[kInventoryBase + 2] == 0xCC && pro[kInventoryBase + 2] == 0 && pro[kInventoryBase + 3] == 0);
    Bytes noisy = plain;
    noisy[kInventoryBase + 2] = 0x5A;
    noisy[kInventoryBase + 3] = 0xA5;
    noisy[0x20EACC + 0x41] = 0x77;
    Bytes noisy_pro;
    CHECK(ConvertPs3ToPro(Reseal(noisy), noisy_pro, report) && noisy_pro == pro);
    Bytes changed = plain;
    changed[kInventoryBase + 1] ^= 1;
    Bytes changed_pro;
    CHECK(ConvertPs3ToPro(Reseal(changed), changed_pro, report) && changed_pro != pro);

    // Converting twice is the same file, and the source is never touched.
    Bytes again;
    CHECK(ConvertPs3ToPro(plain, again, report) && again == pro && plain == MakeConvertiblePs3(3));

    // Only a verified version-36 PS3 plaintext is converted.
    CHECK(!ConvertPs3ToPro(MakeSave(), pro, report) && pro.empty() && !report.converted);
    Bytes bad_crc = plain;
    bad_crc[0x500] ^= 1;
    CHECK(!ConvertPs3ToPro(bad_crc, pro, report) && pro.empty());
    CHECK(!ConvertPs3ToPro(Bytes(10, 0), pro, report));
    for (const uint32_t version : {35u, 37u, 38u}) {
        Bytes other = plain;
        PutBe32(other, 8, version);
        CHECK(!ConvertPs3ToPro(Reseal(other), pro, report) && report.source_version == version && !report.converted);
    }
    Bytes counts = plain;
    PutBe32(counts, 12, 660);
    CHECK(!ConvertPs3ToPro(Reseal(counts), pro, report));
    CHECK(std::string(ImportBlockText(ImportBlock::kPs3UnsupportedLayout)).rfind("Unsupported PS3 save layout", 0) == 0);
}

// Local research modes, never part of a normal run.
//   ps3-layout                 the slot program as a table
//   ps3-convert <folder>       converts every PS3 save folder under <folder> in memory and checks the result
int RunPs3Layout() {
    std::printf("offset\tkind\twidth\n");
    static const char* kNames[] = {"byte", "bool", "u16", "u32", "item_bits"};
    ForEachPs3SlotField([&](size_t offset, Ps3Kind kind) { std::printf("0x%zX\t%s\t%zu\n", offset, kNames[int(kind)], Ps3KindWidth(kind)); });
    return 0;
}

std::string Sha256Hex(const Bytes& bytes) {
    std::string sha;
    uint8_t digest[32];
    if (crypto::Sha256(bytes.data(), bytes.size(), digest)) {
        for (const uint8_t byte : digest) {
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x", byte);
            sha += hex;
        }
    }
    return sha;
}

// Compares one item record, source against converted, without the layout program.
bool ItemPreserved(const Bytes& plain, const Bytes& pro, size_t item) {
    if (ReadBe16(plain, item) != uint16_t(pro[item] | pro[item + 1] << 8)) return false;
    for (size_t effect = 0; effect < 8; ++effect) {
        const size_t at = item + effect * 8;
        if (ReadBe16(plain, at) != uint16_t(pro[at] | pro[at + 1] << 8)) return false;
        if (ReadBe32(plain, at + 4) != ReadLe32(pro, at + 4)) return false;
    }
    const uint32_t source = ReadBe32(plain, item + 0x40), packed = ReadLe32(pro, item + 0x40);
    if ((source >> 7) != (packed & 0x1FFFFFF) || ((source >> 3) & 15) != ((packed >> 25) & 15)) return false;
    for (int bit = 0; bit < 3; ++bit) {
        if (((source >> (2 - bit)) & 1) != ((packed >> (29 + bit)) & 1)) return false;
    }
    if (ReadBe32(plain, item + 0x44) != ReadLe32(pro, item + 0x44) || ReadBe32(plain, item + 0x48) != ReadLe32(pro, item + 0x48)) return false;
    return std::equal(plain.begin() + long(item + 0x4C), plain.begin() + long(item + 0x54), pro.begin() + long(item + 0x4C));
}

int RunPs3Convert(const fs::path& root) {
    std::vector<fs::path> folders;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == "PARAM.PFD" && fs::is_regular_file(it->path().parent_path() / "SAVE0.DAT", ec)) {
            folders.push_back(it->path().parent_path());
        }
    }
    std::sort(folders.begin(), folders.end());
    int decrypted = 0, converted = 0, validated = 0, parsed = 0, stable = 0, characters_kept = 0, items_kept = 0, equipment_kept = 0, padding_zero = 0;
    std::printf("folder\tsha256\n");
    for (const fs::path& folder : folders) {
        Bytes plain;
        if (DecryptPs3Save0(ReadBytes(folder / "PARAM.PFD"), ReadBytes(folder / "SAVE0.DAT"), folder.filename().string(), plain) != Ps3Status::kOk) continue;
        ++decrypted;
        Bytes pro;
        Ps3ConvertReport report;
        if (!ConvertPs3ToPro(plain, pro, report)) continue;
        ++converted;
        std::vector<std::string> problems;
        validated += ValidateSave(pro, problems);
        const SaveModel model = SaveModel::FromBytes(pro);
        parsed += model.valid();
        Bytes again;
        stable += ConvertPs3ToPro(plain, again, report) && again == pro;

        bool characters = model.valid(), inventory = true, equipment = true;
        for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
            const size_t base = slot * kDataSlotStride;
            characters = characters && model.slots()[slot].gold == ReadBe32(plain, base + kDataSlotGoldOffset);
            characters = characters && ReadLe32(pro, base + 0xAF284) == ReadBe32(plain, base + 0xAF284);
            for (size_t c = 0; c < kCharacterCount; ++c) {
                const size_t record = RecordOffset(int(slot), int(c));
                const CharacterView& view = model.slots()[slot].characters[c];
                if (!view.present) continue;
                characters = characters && view.skill_points == ReadBe16(plain, record + kCharSkillPoints) &&
                             view.hp_current == (ReadBe32(plain, record + kCharHpCurrent) & 0xFFFF) &&
                             view.hp_max == (ReadBe32(plain, record + kCharHpMax) & 0xFFFF) && view.level_stored == plain[record + kCharLevel] &&
                             uint8_t(view.klass) == plain[record + kCharClass] && std::equal(plain.begin() + long(record + kCharName), plain.begin() + long(record + kCharName + kCharNameBytes),
                                                                              pro.begin() + long(record + kCharName));
                for (size_t e = 0; e < kEquipmentCount; ++e) equipment = equipment && ItemPreserved(plain, pro, record + kEquipmentRelative + e * kItemSize);
            }
            for (size_t k = 0; k < kInventoryCount; ++k) inventory = inventory && ItemPreserved(plain, pro, base + kInventoryBase + k * kItemSize);
        }
        characters_kept += characters;
        items_kept += inventory;
        equipment_kept += equipment;

        std::vector<uint8_t> typed(kSaveFileSize, 0);
        std::fill(typed.begin(), typed.begin() + 0x50, 1);
        for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
            ForEachPs3SlotField([&](size_t offset, Ps3Kind kind) { std::fill_n(typed.begin() + long(slot * kDataSlotStride + offset), Ps3KindWidth(kind), uint8_t(1)); });
        }
        std::fill(typed.begin() + 0x20EACC, typed.begin() + 0x20EACC + 0x3B, 1);
        std::fill(typed.begin() + 0x20EACC + 0x3B, typed.begin() + 0x20EACC + 0x41, 1);
        std::fill(typed.begin() + 0x20EACC + 0x44, typed.begin() + 0x20EACC + 0x4A, 1);
        bool zero = true;
        for (size_t i = 0; i < pro.size(); ++i) zero = zero && (typed[i] || pro[i] == 0);
        padding_zero += zero;
        std::printf("%s\t%s\n", fs::relative(folder, root, ec).generic_string().c_str(), Sha256Hex(pro).c_str());
    }
    std::printf("PS3 convert: %zu folders, %d decrypted, %d converted, %d valid Pro checksum and layout, %d parsed by SaveModel, %d stable on a second run, "
                "%d with every character field kept, %d with all %zu inventory items kept per slot, %d with every equipped item kept, %d with zero padding\n",
                folders.size(), decrypted, converted, validated, parsed, stable, characters_kept, items_kept, kInventoryCount, equipment_kept, padding_zero);
    return 0;
}

// The launcher's rows open their table inside whatever ID scope is current, so a PushID around a row call ends up popped
// inside the table and EndTable reports "Mismatching PushID/PopID". Pushes may only enclose plain ImGui calls.
void TestImGuiStackDiscipline() {
    const std::string page = Source("launcher/save_page.cpp");
    const auto count = [&](const char* text) {
        size_t n = 0;
        for (size_t at = page.find(text); at != std::string::npos; at = page.find(text, at + 1)) ++n;
        return n;
    };
    for (const char* pair : {"PushID(", "PopID("}) CHECK(count((std::string("ImGui::") + pair).c_str()) >= 1);
    CHECK(count("ImGui::PushID(") == count("ImGui::PopID("));
    CHECK(count("ImGui::BeginDisabled(") == count("ImGui::EndDisabled("));
    CHECK(count("ImGui::BeginCombo(") == count("ImGui::EndCombo("));
    CHECK(count("ImGui::BeginPopup(") + count("ImGui::BeginPopupModal(") == count("ImGui::EndPopup("));
    CHECK(count("ImGui::PushStyleColor(") == count("ImGui::PopStyleColor("));
    CHECK(count("ImGui::PushTextWrapPos(") == count("ImGui::PopTextWrapPos("));
    for (size_t at = page.find("ImGui::PushID("); at != std::string::npos; at = page.find("ImGui::PushID(", at + 1)) {
        const size_t end = page.find("ImGui::PopID(", at);
        CHECK(end != std::string::npos);
        const std::string scope = page.substr(at, end - at);
        CHECK(scope.find("ui.") == std::string::npos && scope.find("return") == std::string::npos);
    }
    // No loop adds an ID scope around a row: the backup rows get their IDs from their own unique labels.
    CHECK(Body(page, "void SavePage::DrawBackups(").find("PushID") == std::string::npos);
    CHECK(Source("launcher/save_page_model.cpp").find("std::map<std::string, int> seen;") != std::string::npos);
}

// Local research mode, never part of a normal run: `ps3-corpus <folder>` decrypts every PS3 save folder under <folder> and
// prints one row each. Nothing is written.
int RunPs3Corpus(const fs::path& root) {
    std::vector<fs::path> folders;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().filename() == "PARAM.PFD" && fs::is_regular_file(it->path().parent_path() / "SAVE0.DAT", ec)) {
            folders.push_back(it->path().parent_path());
        }
    }
    std::sort(folders.begin(), folders.end());
    int decrypted = 0;
    std::printf("folder\tdirectory\tpfd_version\tencrypted_size\tlogical_size\tdecrypted\tmagic_crc\tslots\tcharacters\tgold\tplain_sha256\n");
    for (const fs::path& folder : folders) {
        const Bytes pfd = ReadBytes(folder / "PARAM.PFD"), save = ReadBytes(folder / "SAVE0.DAT");
        Ps3Pfd header;
        const Ps3Status parsed = ParsePfd(pfd, header);
        Ps3Entry entry;
        const bool has_entry = FindPfdEntry(pfd, "SAVE0.DAT", entry) == Ps3Status::kOk;
        Bytes plain;
        const Ps3Status status = DecryptPs3Save0(pfd, save, folder.filename().string(), plain);
        const Ps3Plaintext info = InspectPs3Plaintext(plain);
        decrypted += status == Ps3Status::kOk && info.valid;
        std::string sha;
        uint8_t digest[32];
        if (!plain.empty() && crypto::Sha256(plain.data(), plain.size(), digest)) {
            for (const uint8_t byte : digest) {
                char hex[3];
                std::snprintf(hex, sizeof(hex), "%02x", byte);
                sha += hex;
            }
        }
        std::printf("%s\t%s\t%u\t%zu\t%llu\t%s\t%s\t%d\t%d\t%u/%u/%u\t%s\n", fs::relative(folder, root, ec).generic_string().c_str(),
                    folder.filename().string().c_str(), parsed == Ps3Status::kOk ? header.version : 0u, save.size(),
                    has_entry ? (unsigned long long)entry.size : 0ull, status == Ps3Status::kOk ? "yes" : Ps3StatusText(status),
                    info.valid ? "valid" : "invalid", info.slots, info.characters, info.gold[0], info.gold[1], info.gold[2], sha.c_str());
    }
    std::printf("PS3 corpus: %zu save folders, %d decrypted and valid\n", folders.size(), decrypted);
    return 0;
}

struct Group {
    const char* name;
    std::function<void()> run;
};

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "ps3-corpus") return RunPs3Corpus(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "ps3-convert") return RunPs3Convert(argv[2]);
    if (argc >= 2 && std::string(argv[1]) == "ps3-layout") return RunPs3Layout();
    const fs::path base = fs::temp_directory_path() / "dcp_save_page_tests";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    const Group groups[] = {
        {"save-page-model", [] { TestRegistration(); TestSlots(); TestGuards(); TestPageSource(); TestSavePageUx(); TestImGuiStackDiscipline(); }},
        {"save-page-discovery", [&] { TestDiscoveryAndOverview(base); TestInstalledGameScope(base); }},
        {"save-page-backup", [&] { TestBackups(base); TestBackupMetadataSafety(base); TestBackupListing(base); TestBackupDelete(base); }},
        {"save-page-export", [&] { TestExport(base); }},
        {"save-page-import", [&] { TestImport(base); TestTitleTables(); TestImportSources(base); }},
        {"save-page-archives", [&] { TestArchives(base); TestPlatformDiagnostics(); }},
        {"save-page-edit", [&] { TestEditSaveFields(base); TestEditSaveSafety(base); }},
        {"save-page-ps3", [&] { TestPs3Crypto(); TestPfdParsing(); TestPs3Decryption(); TestPs3Plaintext(); TestPs3Import(base); TestImportModes(base); }},
        {"save-page-ps3-layout", [] { TestPs3Layout(); TestPs3Conversion(); }},
        {"save-page-restore", [&] { TestRestore(base); TestInstalledDestination(base); }},
        {"save-page-editor-contract", [&] { TestEditorInvalidation(base); }},
    };
    for (const Group& group : groups) {
        const int checks = g_checks, failures = g_failures;
        const auto started = std::chrono::steady_clock::now();
        group.run();
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const testprogress::State state = g_failures != failures ? testprogress::State::kFail : testprogress::State::kPass;
        std::printf("%s\n", testprogress::FormatGroupLine(group.name, state, g_checks - checks, ms).c_str());
        std::fflush(stdout);
    }
    fs::remove_all(base, ec);
    std::printf("save page tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
