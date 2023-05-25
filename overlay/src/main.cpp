#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one
#include <tesla.hpp>    // The Tesla Header
#include <string_view>
#include "minIni/minIni.h"

namespace {

constexpr auto CONFIG_PATH = "/config/sys-patch/config.ini";
constexpr auto LOG_PATH = "/config/sys-patch/log.ini";

auto does_file_exist(const char* path) -> bool {
    Result rc{};
    FsFileSystem fs{};
    FsFile file{};
    char path_buf[FS_MAX_PATH]{};

    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return false;
    }

    strcpy(path_buf, path);
    rc = fsFsOpenFile(&fs, path_buf, FsOpenMode_Read, &file);
    fsFileClose(&file);
    fsFsClose(&fs);
    return R_SUCCEEDED(rc);
}

// creates a directory, non-recursive!
auto create_dir(const char* path) -> bool {
    Result rc{};
    FsFileSystem fs{};
    char path_buf[FS_MAX_PATH]{};

    if (R_FAILED(fsOpenSdCardFileSystem(&fs))) {
        return false;
    }

    strcpy(path_buf, path);
    rc = fsFsCreateDirectory(&fs, path_buf);
    fsFsClose(&fs);
    return R_SUCCEEDED(rc);
}

struct ConfigEntry {
    const char* const section;
    const char* const key;
    bool value;

    ConfigEntry(const char* _section, const char* _key, bool default_value) :
        section{_section}, key{_key}, value{default_value} {}

    void load_value_from_ini() {
        this->value = ini_getbool(this->section, this->key, this->value, CONFIG_PATH);
    }

    auto create_list_item(const char* text) {
        auto item = new tsl::elm::ToggleListItem(text, value);
        item->setStateChangedListener([this](bool new_value){
            this->value = new_value;
            ini_putl(this->section, this->key, this->value, CONFIG_PATH);
        });
        return item;
    }
};

class GuiMain final : public tsl::Gui {
public:
    GuiMain() { }

    // Called when this Gui gets loaded to create the UI
    // Allocate all elements on the heap. libtesla will make sure to clean them up when not needed anymore
    tsl::elm::Element* createUI() override {
        create_dir("/config/");
        create_dir("/config/sys-patch/");

        config_patch_sysmmc.load_value_from_ini();
        config_patch_emummc.load_value_from_ini();
        config_logging.load_value_from_ini();

        auto frame = new tsl::elm::OverlayFrame("sys-patch", "v1.2.0");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Options"));
        list->addItem(config_patch_sysmmc.create_list_item("Patch SysMMC"));
        list->addItem(config_patch_emummc.create_list_item("Patch EmuMMC"));
        list->addItem(config_logging.create_list_item("Logging"));

        if (does_file_exist(LOG_PATH)) {
            struct CallbackUser {
                tsl::elm::List* list;
                std::string last_section;
            } callback_userdata{list};

            ini_browse([](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData){
                auto user = (CallbackUser*)UserData;
                if (!std::strcmp("Skipped", Value)) {
                    return 1;
                }

                if (user->last_section != Section) {
                    user->last_section = Section;
                    user->list->addItem(new tsl::elm::CategoryHeader("Log: " + user->last_section));
                }

                #define F(x) ((x) >> 4) // 8bit -> 4bit
                constexpr tsl::Color colour_syspatch{F(0), F(255), F(200), F(255)};
                constexpr tsl::Color colour_file{F(255), F(177), F(66), F(255)};
                constexpr tsl::Color colour_unpatched{F(250), F(90), F(58), F(255)};
                #undef F

                std::string_view value{Value};
                if (value.starts_with("Patched")) {
                    if (value.ends_with("(sys-patch)")) {
                        user->list->addItem(new tsl::elm::ListItem(Key, "Patched", colour_syspatch));
                    } else {
                        user->list->addItem(new tsl::elm::ListItem(Key, "Patched", colour_file));
                    }
                } else if (value.starts_with("Unpatched")) {
                    user->list->addItem(new tsl::elm::ListItem(Key, Value, colour_unpatched));
                }

                return 1;
            }, &callback_userdata, LOG_PATH);
        } else {

        }

        frame->setContent(list);
        return frame;
    }

    ConfigEntry config_patch_sysmmc{"options", "patch_sysmmc", true};
    ConfigEntry config_patch_emummc{"options", "patch_emummc", true};
    ConfigEntry config_logging{"options", "patch_logging", true};
};

// libtesla already initialized fs, hid, pl, pmdmnt, hid:sys and set:sys
class SysPatchOverlay final : public tsl::Overlay {
public:
    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }
};

} // namespace

int main(int argc, char **argv) {
    return tsl::loop<SysPatchOverlay>(argc, argv);
}
