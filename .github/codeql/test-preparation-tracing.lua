-- Ensure the exception cannot suppress tracing for ordinary compilers or tools.
OperatingSystem = "linux"
local original = function() return {trace = true} end
function GetRegisteredMatchers(language)
    assert(language == "cpp")
    return {original}
end
dofile(".github/codeql/sai-preparation-tracing.lua")
local matchers = RegisterExtraConfig().cpp
assert(#matchers == 2 and matchers[2] == original, "Original C++ matcher was lost")
local python = "/execroot/_main/external/rules_python++python+python_3_11_x86_64-unknown-linux-gnu/bin/python3"
local count = 0
local function check(argv, excluded, name, path)
    local result = matchers[1](name or "python3", path or python, {argv = argv}, "cpp")
    assert((result ~= nil and result.trace == false) == excluded,
           "Unexpected tracing decision for " .. table.concat(argv, " "))
    count = count + 1
end
local function copy(values)
    local result = {}
    for index, value in ipairs(values) do result[index] = value end
    return result
end
for _, architecture in ipairs({"amd64", "arm64"}) do
    for _, spec in ipairs({
        {"external/sai+/bazel/prepare_deb_tools.py", "/external/sai+/bazel/sai_generator_tools", "--deb", "a.deb"},
        {"external/sonic-build-infra+/tools/build_tools/prepare_runtime.py", "/external/sonic-build-infra+/tools/build_tools/runtime_" .. architecture, "--tar", "content.tar.gz"},
        {"external/sonic-build-infra+/tools/build_tools/prepare_runtime.py", "/external/sonic-build-infra+/tools/build_tools/configure_runtime_" .. architecture, "--tar", "data.tar.xz"},
    }) do
        local argv = {spec[1], "--out", "bazel-out/exec/bin" .. spec[2], "--architecture", architecture}
        if spec[3] == "--tar" then
            local profile = string.find(spec[2], "configure_runtime_") and "configure" or "documentation"
            for _, value in ipairs({"--profile", profile, "--launcher",
                "bazel-out/exec/bin/external/sonic-build-infra+/tools/build_tools/runtime_launcher"}) do
                table.insert(argv, value)
            end
        end
        table.insert(argv, spec[3])
        table.insert(argv, spec[4])
        check(argv, true)
        if spec[3] == "--tar" then
            for _, profile in ipairs({"kerberos", "foreign"}) do
                local changed = copy(argv)
                changed[7] = profile
                check(changed, false)
            end
            local changed = copy(argv)
            changed[9] = "/usr/bin/g++"
            check(changed, false)
            changed = copy(argv)
            changed[10], changed[11] = "--source", "source.cc"
            check(changed, false)
        end
        local absolute = copy(argv)
        absolute[1] = "/execroot/_main/" .. absolute[1]
        check(absolute, true, "python3.11", python .. ".11")
        check(argv, false, "g++", "/usr/bin/g++")
        check(argv, false, "python3", "/usr/bin/python3")
        for _, change in ipairs({
            {1, "external/sonic-build-infra+/tools/build_tools/launcher.py"},
            {1, "external/sai+/bazel/generate_metadata.py"},
            {1, "external/other+/prepare_runtime.py"},
            {2, "--compiler"}, {3, "bazel-out/exec/bin/unrelated"},
            {4, "--target"}, {5, "unsupported"},
            {6, "--source"}, {7, "source.cc"},
        }) do
            local altered = copy(argv)
            altered[change[1]] = change[2]
            check(altered, false)
        end
        local extra = copy(argv)
        table.insert(extra, "--compiler")
        table.insert(extra, "/usr/bin/g++")
        check(extra, false)
        OperatingSystem = "windows"
        check(argv, false)
        OperatingSystem = "linux"
    end
end
check({}, false)
print("Tool-preparation tracing checks passed: " .. count)
