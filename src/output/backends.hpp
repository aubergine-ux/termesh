// Constructors for the individual backends (see backend.hpp for the
// interface, and makeBackend() for the usual way to get one).
#pragma once

#include <memory>

#include "output/backend.hpp"

namespace termesh {

std::unique_ptr<Backend> makeHalfBlockBackend(ColorDepth color);
std::unique_ptr<Backend> makeBrailleBackend(ColorDepth color);
std::unique_ptr<Backend> makeAsciiBackend(ColorDepth color);
std::unique_ptr<Backend> makeKittyBackend();
std::unique_ptr<Backend> makeSixelBackend();

}  // namespace termesh
