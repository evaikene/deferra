#include "commands/commands_priv.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string_view>

using jb::jobuctl::detail::escape_human;
using jb::jobuctl::detail::format_human_name;

namespace {

struct NameExample {
    std::string_view stored;
    std::string_view rendered;
};

void check_names(std::initializer_list<NameExample> examples)
{
    for (auto const& example : examples) {
        CAPTURE(example.stored, example.rendered);
        CHECK(format_human_name(example.stored) == example.rendered);
    }
}

} // namespace

TEST_CASE("human names preserve ordinary text and internal spaces", "[jobuctl][output]")
{
    check_names({
        {.stored = "Default",                  .rendered = "Default"                 },
        {.stored = "My Queue",                 .rendered = "My Queue"                },
        {.stored = "two  internal  spaces",    .rendered = "two  internal  spaces"   },
        {.stored = "Caf\u00e9",                .rendered = "Caf\u00e9"               },
        {.stored = "e\u0301",                  .rendered = "e\u0301"                 },
        {.stored = "\u961f\u5217",             .rendered = "\u961f\u5217"            },
        {.stored = "\u0635\u0628\u0627\u062d", .rendered = "\u0635\u0628\u0627\u062d"},
        {.stored = "jobs \U0001f680",          .rendered = "jobs \U0001f680"         },
    });
}

TEST_CASE("human names delimit empty names and boundary spaces", "[jobuctl][output]")
{
    check_names({
        {.stored = "",          .rendered = "''"         },
        {.stored = " ",         .rendered = "' '"        },
        {.stored = "Default ",  .rendered = "'Default '" },
        {.stored = " Default",  .rendered = "' Default'" },
        {.stored = " Default ", .rendered = "' Default '"},
        {.stored = "My Queue ", .rendered = "'My Queue '"},
    });
}

TEST_CASE("human names escape literal quotes and backslashes without ambiguity", "[jobuctl][output]")
{
    check_names({
        {.stored = "O'Reilly",      .rendered = R"('O\'Reilly')"   },
        {.stored = R"(say "yes")",  .rendered = R"('say \"yes\"')" },
        {.stored = R"(path\name)",  .rendered = R"('path\\name')"  },
        {.stored = " Default' ",    .rendered = R"(' Default\' ')" },
        {.stored = R"('Default ')", .rendered = R"('\'Default \'')"},
        {.stored = "Default\t",     .rendered = R"('Default\t')"   },
        {.stored = R"(Default\t)",  .rendered = R"('Default\\t')"  },
        {.stored = "Default\n",     .rendered = R"('Default\n')"   },
        {.stored = R"(Default\n)",  .rendered = R"('Default\\n')"  },
        {.stored = "A\u00a0B",      .rendered = R"('A\u00A0B')"    },
        {.stored = R"(A\u00A0B)",   .rendered = R"('A\\u00A0B')"   },
    });
}

TEST_CASE("human names share terminal control escaping with free-form output", "[jobuctl][output]")
{
    check_names({
        {.stored = "A\tB",                      .rendered = R"('A\tB')"     },
        {.stored = "A\nB",                      .rendered = R"('A\nB')"     },
        {.stored = "A\rB",                      .rendered = R"('A\rB')"     },
        {.stored = "Default\t",                 .rendered = R"('Default\t')"},
        {.stored = "Default\n",                 .rendered = R"('Default\n')"},
        {.stored = std::string_view{"A\0B", 3}, .rendered = R"('A\x00B')"   },
        {.stored = "A\aB",                      .rendered = R"('A\x07B')"   },
        {.stored = "\x1b[31m",                  .rendered = R"('\x1B[31m')" },
        {.stored = "A\x7f",                     .rendered = R"('A\x7F')"    },
        {.stored = "A\u0085B",                  .rendered = R"('A\u0085B')" },
        {.stored = "A\u009bB",                  .rendered = R"('A\u009BB')" },
    });

    CHECK(escape_human("\x1b\n\r\t\x7f\u009b") == R"(\x1B\n\r\t\x7F\u009B)");
}

TEST_CASE("human names reveal non-ASCII whitespace even inside a name", "[jobuctl][output]")
{
    check_names({
        {.stored = "Default\u00a0", .rendered = R"('Default\u00A0')"},
        {.stored = "\u00a0Default", .rendered = R"('\u00A0Default')"},
        {.stored = "A\u00a0B",      .rendered = R"('A\u00A0B')"     },
        {.stored = "A\u1680B",      .rendered = R"('A\u1680B')"     },
        {.stored = "A\u2000B",      .rendered = R"('A\u2000B')"     },
        {.stored = "A\u2003B",      .rendered = R"('A\u2003B')"     },
        {.stored = "A\u2007B",      .rendered = R"('A\u2007B')"     },
        {.stored = "A\u200aB",      .rendered = R"('A\u200AB')"     },
        {.stored = "A\u2028B",      .rendered = R"('A\u2028B')"     },
        {.stored = "A\u2029B",      .rendered = R"('A\u2029B')"     },
        {.stored = "A\u202fB",      .rendered = R"('A\u202FB')"     },
        {.stored = "A\u205fB",      .rendered = R"('A\u205FB')"     },
        {.stored = "A\u3000B",      .rendered = R"('A\u3000B')"     },
    });
}

TEST_CASE("human names reveal default-ignorable characters and bidi controls", "[jobuctl][output]")
{
    check_names({
        {.stored = "A\u00adB",     .rendered = R"('A\u00ADB')"    },
        {.stored = "A\u034fB",     .rendered = R"('A\u034FB')"    },
        {.stored = "A\u061cB",     .rendered = R"('A\u061CB')"    },
        {.stored = "A\u115fB",     .rendered = R"('A\u115FB')"    },
        {.stored = "A\u1160B",     .rendered = R"('A\u1160B')"    },
        {.stored = "A\u17b4B",     .rendered = R"('A\u17B4B')"    },
        {.stored = "A\u180eB",     .rendered = R"('A\u180EB')"    },
        {.stored = "A\u200bB",     .rendered = R"('A\u200BB')"    },
        {.stored = "A\u200cB",     .rendered = R"('A\u200CB')"    },
        {.stored = "A\u200dB",     .rendered = R"('A\u200DB')"    },
        {.stored = "A\u200eB",     .rendered = R"('A\u200EB')"    },
        {.stored = "A\u200fB",     .rendered = R"('A\u200FB')"    },
        {.stored = "A\u202aB",     .rendered = R"('A\u202AB')"    },
        {.stored = "A\u202eB",     .rendered = R"('A\u202EB')"    },
        {.stored = "A\u2060B",     .rendered = R"('A\u2060B')"    },
        {.stored = "A\u2066B",     .rendered = R"('A\u2066B')"    },
        {.stored = "A\u2069B",     .rendered = R"('A\u2069B')"    },
        {.stored = "A\u3164B",     .rendered = R"('A\u3164B')"    },
        {.stored = "A\ufe0fB",     .rendered = R"('A\uFE0FB')"    },
        {.stored = "A\ufeffB",     .rendered = R"('A\uFEFFB')"    },
        {.stored = "A\uffa0B",     .rendered = R"('A\uFFA0B')"    },
        {.stored = "A\U0001bca0B", .rendered = R"('A\U0001BCA0B')"},
        {.stored = "A\U0001d173B", .rendered = R"('A\U0001D173B')"},
        {.stored = "A\U000e0067B", .rendered = R"('A\U000E0067B')"},
        {.stored = "A\U000e0100B", .rendered = R"('A\U000E0100B')"},
    });
}

TEST_CASE("human names escape malformed UTF-8 bytes without swallowing surrounding text", "[jobuctl][output]")
{
    check_names({
        {.stored = "A\x80!",             .rendered = R"('A\x80!')"            },
        {.stored = "A\xc0\xaf!",         .rendered = R"('A\xC0\xAF!')"        },
        {.stored = "A\xe0\x80\xaf!",     .rendered = R"('A\xE0\x80\xAF!')"    },
        {.stored = "A\xed\xa0\x80!",     .rendered = R"('A\xED\xA0\x80!')"    },
        {.stored = "A\xf0\x9f!",         .rendered = R"('A\xF0\x9F!')"        },
        {.stored = "A\xf4\x90\x80\x80!", .rendered = R"('A\xF4\x90\x80\x80!')"},
        {.stored = "A\xc3",              .rendered = R"('A\xC3')"             },
    });
}

TEST_CASE("free-form human escaping retains its existing presentation", "[jobuctl][output]")
{
    auto const text = std::string_view{" Default ' \" \\ \u00a0\u200b \U0001f680 "};
    CHECK(escape_human(text) == text);
    CHECK(escape_human({}).empty());
    CHECK(escape_human(std::string_view{"A\0B", 3}) == R"(A\x00B)");
    CHECK(escape_human("A\xc3") == "A\xc3");
}
