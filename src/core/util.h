// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

std::string calcFileMD5();

namespace fkShell {
  enum TextColor {
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
  };
  enum TextType {
    NoType,
    Bold,
    UnderLine
  };
}

std::string Color(const std::string &raw, fkShell::TextColor color,
                                  fkShell::TextType type = fkShell::NoType);

std::string toHex(std::string_view sv);

std::string qCompress_std(const std::string_view &data, int level = -1);
std::string qUncompress_std(const std::string_view &data);
