#include "program/galaxy_config.hpp"

namespace galaxycoop::config {
namespace {
constexpr BuildIdentity kSelected{Game::Smg1, "76F1B4D83962B29C88D9F6D68AC70E233BB66F4C852FAAB681D0BC6F63805938"};
}

const BuildIdentity& GetBuildIdentity() {
    return kSelected;
}

}  // namespace galaxycoop::config