#include "install_source.h"

#include "localization/ui_text.h"

namespace dcp {

const char* InstallSourceLabel(InstallSource source) {
    return Tr(source == InstallSource::kPackage ? "From PKG..." : "From extracted folder...");
}

std::string ChooseInstallSource(InstallSource source, const SourceChoosers& choosers) {
    if (source == InstallSource::kFolder) {
        return choosers.folder ? choosers.folder(Tr("Select your Dragon's Crown Pro extracted game folder")) : std::string();
    }
    return choosers.file ? choosers.file(Tr("Select your Dragon's Crown Pro package"),
                                         {{Tr("PS4 packages (*.pkg)"), "*.pkg"}})
                         : std::string();
}

} // namespace dcp
