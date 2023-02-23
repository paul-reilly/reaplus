set_project("reaplus")
set_version("0.1.0")
add_rules("mode.debug", "mode.release", "mode.asan", "mode.tsan", "mode.ubsan")
set_languages("c++17")

includes("external/**/xmake.lua")

add_requires("spdlog", "concurrentqueue")
add_requires("boost", { configs = { filesystem = true, exception = true}})

target("reaplus")
    set_kind("static")
    add_files("src/**/*.cpp", "src/*.cpp")
    add_defines("NOMINMAX")
    add_includedirs("./include", { public = true})
    add_includedirs("external/reaper",
        "external/WDL/",
        "external/RxCpp/Rx/v2/src")
    add_packages("boost", "spdlog", "concurrentqueue")
    add_deps("helgoboss-midi", "helgoboss-learn")
 


