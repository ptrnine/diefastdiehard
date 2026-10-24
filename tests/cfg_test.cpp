#include <catch2/catch_test_macros.hpp>

#include <iostream>
#include <fstream>
#include "base/cfg.hpp"
#include "base/vec_math.hpp"

using namespace std::string_view_literals;

static constexpr auto section0 =
R"([section0]
some = text
)"sv;

static constexpr auto section1 =
R"([section1]
health = 200
power  = 100
name   = super car

)"sv;

static constexpr auto section2 =
R"([section2]
size         = 400 200
texture path = path/to/texture
)"sv;

static constexpr auto section3_part0 =
R"([section3]
key1 = val1
key2 = val2
)"sv;

static constexpr auto section3_part1 =
R"(key3 = val3
key4 = val4
)"sv;

static constexpr auto global_section =
R"(global key = global value
)"sv;

auto reinit_cfg() {
    using namespace dfdh;

    fs::create_directories("test_data/configs/");

    {
        auto ofs = std::ofstream("test_data/configs/test.cfg");
        ofs << "#include test2.cfg\n" << section1 << section2 << section3_part0 << "#include test3.cfg";
    }
    {
        auto ofs = std::ofstream("test_data/configs/test2.cfg");
        ofs << global_section << section0;
    }
    {
        auto ofs = std::ofstream("test_data/configs/test3.cfg");
        ofs << section3_part1;
    }

    auto cfg = dfdh::cfg("test_data/configs/test.cfg");
    return std::tuple<dfdh::cfg,
                      cfg_section<false>&,
                      cfg_section<false>&,
                      cfg_section<false>&,
                      cfg_section<false>&,
                      cfg_section<false>&,
                      cfg_file_node&,
                      cfg_file_node&,
                      cfg_file_node&>(std::move(cfg),
                                      cfg.get_section(""_sect),
                                      cfg.get_section("section0"_sect),
                                      cfg.get_section("section1"_sect),
                                      cfg.get_section("section2"_sect),
                                      cfg.get_section("section3"_sect),
                                      cfg.get_file("test_data/configs/test.cfg"_file),
                                      cfg.get_file("test_data/configs/test2.cfg"_file),
                                      cfg.get_file("test_data/configs/test3.cfg"_file));
}

auto reread_and_compare(const dfdh::cfg& cfg) {
    auto new_cfg = dfdh::cfg("test_data/configs/test.cfg");
    REQUIRE(cfg == new_cfg);
}

TEST_CASE("Cfg test") {
    using namespace dfdh;

    auto&& [cfg, global_sect, sect0, sect1, sect2, sect3, file0, file1, file2] = reinit_cfg();

    REQUIRE(dfdh::format("{}", cfg) ==
            build_string(global_section, section0, "\n"sv, section1, section2, section3_part0, section3_part1));
    REQUIRE(dfdh::format("{}", sect0) == build_string(section0, "\n"sv));
    REQUIRE(dfdh::format("{}", sect1) == std::string(section1));
    REQUIRE(dfdh::format("{}", sect2) == std::string(section2));
    REQUIRE(dfdh::format("{}", cfg) == dfdh::format("{}", file0));
    REQUIRE(dfdh::format("{}", file1) == build_string(global_section, section0));

    REQUIRE(cfg.list_sections() == std::set{""s, "section0"s, "section1"s, "section2"s, "section3"s});
    REQUIRE(global_sect.list_keys() == std::set{"global key"s});
    REQUIRE(sect0.list_keys() == std::set{"some"s});
    REQUIRE(sect1.list_keys() == std::set{"health"s, "power"s, "name"s});
    REQUIRE(sect2.list_keys() == std::set{"size"s, "texture path"s});

    REQUIRE(global_sect.get<std::string>("global key").value() == "global value");
    REQUIRE(sect0.get<std::string>("some").value() == "text");
    REQUIRE(sect1.get<int>("health").value() == 200);
    REQUIRE(sect1.get<int>("power").value() == 100);
    REQUIRE(sect1.get<std::string>("name").value() == "super car");
    REQUIRE(sect2.get<std::tuple<int, int>>("size").value() == std::tuple{400, 200});
    REQUIRE(sect2.get<std::string>("texture path").value() == "path/to/texture");

    SECTION("edit values") {
        sect0.get<std::string>("some").set("new some value");
        REQUIRE(file1.commit_required);
        REQUIRE(!file0.commit_required);

        global_sect.set("global key", "new global value"s);
        REQUIRE(file1.commit_required);
        REQUIRE(!file0.commit_required);

        sect1.raw_set("health", "400");
        sect1.access<int>("power").raw_set("1000");
        sect1.try_get<std::string>("name").value().set("new car name");
        REQUIRE(file0.commit_required);
        REQUIRE(file1.commit_required);

        cfg.commit();
        reread_and_compare(cfg);

        REQUIRE(sect0.get<std::string>("some").value() == "new some value");
        REQUIRE(global_sect.get<std::string>("global key").value() == "new global value");
        REQUIRE(sect1.get<int>("health").value() == 400);
        REQUIRE(sect1.get<int>("power").value() == 1000);
        REQUIRE(sect1.get<std::string>("name").value() == "new car name");

        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nsome = new some value\n\n");
        REQUIRE(dfdh::format("{}", sect1) == "[section1]\nhealth = 400\npower  = 1000\nname   = new car name\n\n");
        REQUIRE(dfdh::format("{}", global_sect) == "global key = new global value\n");
        REQUIRE(dfdh::format("{}", sect2) == std::string(section2));
    }

    SECTION("remove/insert values") {
        REQUIRE(sect0.delete_key("some"));
        REQUIRE(sect0.get_values().empty());

        auto v1 = sect0.set("newkey", 1337);

        REQUIRE(sect0.get_values().size() == 1);
        REQUIRE(sect0.get<int>("newkey").has_value());
        REQUIRE(sect0.get<int>("newkey").value() == 1337);
        REQUIRE(v1.value() == 1337);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nnewkey = 1337\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.get<int>("newkey").clear();
        REQUIRE(sect0.get_values().size() == 1);
        REQUIRE(!sect0.get<int>("newkey").has_value());
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nnewkey =\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.get<int>("newkey").set(1338);
        REQUIRE(sect0.get_values().size() == 1);
        REQUIRE(sect0.get<int>("newkey").has_value());
        REQUIRE(sect0.get<int>("newkey").value() == 1338);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nnewkey = 1338\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.get<std::string>("newkey").set(""); // must be same as clear
        REQUIRE(!sect0.get<int>("newkey").has_value());

        cfg.commit();
        reread_and_compare(cfg);

        sect0.set("newkey", 1339);
        REQUIRE(sect0.get_values().size() == 1);
        REQUIRE(sect0.get<int>("newkey").has_value());
        REQUIRE(sect0.get<int>("newkey").value() == 1339);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nnewkey = 1339\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.delete_key("newkey");
        REQUIRE(sect0.get_values().size() == 0);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\n\n");

        cfg.commit();
        reread_and_compare(cfg);
    }

    SECTION("key sort") {
        REQUIRE(sect0.delete_key("some"));
        REQUIRE(sect0.get_values().empty());

        cfg_settings().insert = cfg_settings_insert::lexicographical_compare_key;
        cfg_settings().indent = cfg_settings_indentation::spaces;

        auto v1 = sect0.set("some_long_key", 1234);
        auto v2 = sect0.set("another_key", std::string("value1"));
        auto v3 = sect0.set("zlast_key", 123.123);
        auto v4 = sect0.set("x", 1);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nanother_key   = value1\nsome_long_key = 1234\nx             = "
                                       "1\nzlast_key     = 123.123\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        REQUIRE(sect0.get_values().size() == 4);
        REQUIRE(sect0.get<int>("some_long_key").value() == 1234);
        REQUIRE(sect0.get<std::string>("another_key").value() == "value1");
        REQUIRE(approx_equal(sect0.get<double>("zlast_key").value(), 123.123, 0.0000001));
        REQUIRE(sect0.get<int>("x").value() == 1);

        REQUIRE(v1.value() == 1234);
        REQUIRE(v2.value() == "value1");
        REQUIRE(approx_equal(v3.value(), 123.123, 0.0000001));
        REQUIRE(v4.value() == 1);

        sect0.get<int>("some_long_key").clear();
        sect0.get<std::string>("another_key").clear();
        sect0.get<double>("zlast_key").clear();
        sect0.get<int>("x").clear();

        REQUIRE(!v1.has_value());
        REQUIRE(!v2.has_value());
        REQUIRE(!v3.has_value());
        REQUIRE(!v4.has_value());
        REQUIRE(dfdh::format("{}", sect0) ==
                "[section0]\nanother_key   =\nsome_long_key =\nx             =\nzlast_key     =\n\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.get<int>("some_long_key").set(1111);
        sect0.get<bool>("another_key").set(false);
        sect0.get<double>("zlast_key").set(999.999);
        sect0.get<int>("x").set(-9876);

        cfg.commit();
        reread_and_compare(cfg);

        REQUIRE(v1.value() == 1111);
        REQUIRE(v2.value() == "false");
        REQUIRE(approx_equal(v3.value(), 999.999, 0.0000001));
        REQUIRE(v4.value() == -9876);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\nanother_key   = false\nsome_long_key = 1111\nx             = "
                                       "-9876\nzlast_key     = 999.999\n\n");

        sect0.get<int>("some_long_key").clear();

        cfg.commit();
        reread_and_compare(cfg);

        sect0.clear();
        REQUIRE(sect0.get_values().empty());
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\n");

        cfg.commit();
        reread_and_compare(cfg);

        sect0.set<std::string>("d", "D");
        sect0.set<std::string>("b", "B");
        sect0.set<std::string>("a", "A");
        sect0.set<std::string>("c", "C");

        REQUIRE(sect0.get_values().size() == 4);
        REQUIRE(dfdh::format("{}", sect0) == "[section0]\na = A\nb = B\nc = C\nd = D\n");

        //for (auto p = cfg.head_node(); p; p = p->next.get()) {
        //    std::cerr << enum_name(p->tk.type) << ": (" << p->tk.value << ")  file: " << (p->file ? p->file->path : "null") << std::endl;
        //}

        cfg.commit();
        reread_and_compare(cfg);

        cfg.remove_section("section0"_sect);
        REQUIRE(!cfg.try_get_section("section0"_sect));

        cfg.commit();
        reread_and_compare(cfg);

        cfg_settings().insert = cfg_settings_insert::after_section_declaration;
        cfg_settings().indent = cfg_settings_indentation::off;
    }

    SECTION("create sections 1") {
        cfg.create_section("section"_sect).set("test", 100);
        cfg.create_section("section01"_sect).set("test", 200);
        cfg.create_section("section21"_sect).set("test", 300);
        cfg.create_section("section31"_sect).set("test", 400);

        cfg.commit();
        reread_and_compare(cfg);

        REQUIRE(dfdh::format("{}", cfg) == build_string(global_section,
                                                  "\n[section]\ntest = 100\n\n"sv,
                                                  section0,
                                                  "\n\n[section01]\ntest = 200\n\n"sv,
                                                  section1,
                                                  section2,
                                                  "\n[section21]\ntest = 300\n\n"sv,
                                                  section3_part0,
                                                  section3_part1,
                                                  "\n[section31]\ntest = 400\n\n"sv));

        REQUIRE(dfdh::format("{}", file0) == dfdh::format("{}", cfg));
        REQUIRE(dfdh::format("{}", file1) == build_string(global_section, "\n[section]\ntest = 100\n\n"sv, section0));
        REQUIRE(dfdh::format("{}", file2) == build_string(section3_part1, "\n[section31]\ntest = 400\n\n"sv));
    }

    SECTION("create sections 2") {
        cfg.create_section("section"_sect, "test_data/configs/test.cfg"_file).set("test", 100);
        cfg.create_section("section01"_sect).set("test", 200);
        cfg.create_section("section21"_sect).set("test", 300);
        cfg.create_section("section31"_sect, "test_data/configs/test.cfg"_file).set("test", 400);

        cfg.commit();
        reread_and_compare(cfg);

        REQUIRE(dfdh::format("{}", cfg) == build_string(global_section,
                                                  section0,
                                                  "\n\n[section]\ntest = 100\n"sv,
                                                  "\n[section01]\ntest = 200\n\n"sv,
                                                  section1,
                                                  section2,
                                                  "\n[section21]\ntest = 300\n\n"sv,
                                                  section3_part0,
                                                  section3_part1,
                                                  "\n\n[section31]\ntest = 400\n\n"sv));

        REQUIRE(dfdh::format("{}", file0) == dfdh::format("{}", cfg));
        REQUIRE(dfdh::format("{}", file1) == build_string(global_section, section0));
        REQUIRE(dfdh::format("{}", file2) == section3_part1);
    }
}

TEST_CASE("Empty cfg files") {
    using namespace dfdh;

    fs::create_directories("test_data/configs/");

    // A completely empty file is a valid (empty) config
    {
        std::ofstream("test_data/configs/empty.cfg").close();
        auto cfg = dfdh::cfg("test_data/configs/empty.cfg");
        REQUIRE(cfg.list_sections() == std::set<std::string>{});
    }

    // An empty included file must not unbalance the file stack
    {
        std::ofstream("test_data/configs/empty_incl.cfg")
            << "[sect]"s
            << "a = 1\n"s
            << "#include empty.cfg\n"s
            << "b = 2\n"s;
        auto cfg = dfdh::cfg("test_data/configs/empty_incl.cfg");
        auto sect = cfg.get_section("sect"_sect);
        REQUIRE(sect.get<int>("a").value() == 1);
        REQUIRE(sect.get<int>("b").value() == 2);
    }

    // A comments-only file has no tokens either
    {
        std::ofstream("test_data/configs/comments_only.cfg") << "# just a comment\n";
        auto cfg = dfdh::cfg("test_data/configs/comments_only.cfg");
        REQUIRE(cfg.list_sections() == std::set<std::string>{});
    }
}
