-- SKSE-Bridge: legacy plugin host. No CommonLib dependency (see docs/AUDIT.md, option 1).
set_xmakever('2.8.2')

set_project('SKSE-Bridge')
set_version('0.2.0')
set_languages('c++20')
set_warnings('allextra')
set_encodings('utf-8')

add_rules('mode.debug', 'mode.releasedbg', 'mode.release')
set_defaultmode('releasedbg')

-- Accepted for build-script compatibility (--skyrim_se=y --skyrim_ae=y --skyrim_vr=y).
-- The host has no runtime-specific code, so these change nothing.
for _, name in ipairs({ 'skyrim_se', 'skyrim_ae', 'skyrim_vr' }) do
    option(name)
        set_default(false)
        set_showmenu(true)
        set_description('Ignored by SKSE-Bridge (runtime independent)')
    option_end()
end

target('SKSE-Bridge')
    set_kind('shared')
    set_arch('x64')
    add_files('src/*.cpp')
    add_headerfiles('src/*.h')
    add_defines('UNICODE', '_UNICODE')
    add_syslinks('kernel32', 'user32', 'version', 'bcrypt', 'shell32', 'ole32', 'uuid')
    add_cxxflags('cl::/EHsc', 'cl::/permissive-', 'cl::/Zc:preprocessor')
    -- static CRT: the host must load without a VC++ redistributable dependency of its own
    set_runtimes(is_mode('debug') and 'MTd' or 'MT')

    after_build(function(target)
        local mods = os.getenv('XSE_TES5_MODS_PATH')
        local game = os.getenv('XSE_TES5_GAME_PATH')
        local dest = mods and path.join(mods, 'SKSE-Bridge', 'SKSE', 'Plugins')
            or (game and path.join(game, 'Data', 'SKSE', 'Plugins'))
        if dest then
            os.mkdir(dest)
            os.trycp(target:targetfile(), dest)
            if not os.isfile(path.join(dest, 'LegacyHost.ini')) then
                os.trycp(path.join(os.projectdir(), 'dist', 'LegacyHost.ini'), dest)
            end
        end
    end)
