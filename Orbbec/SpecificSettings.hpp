#pragma once
#include <score/tools/std/StringHash.hpp>

#include <verdigris>

namespace Orbbec
{
struct SpecificSettings
{
  int control{1234};
};
}

Q_DECLARE_METATYPE(Orbbec::SpecificSettings)
W_REGISTER_ARGTYPE(Orbbec::SpecificSettings)