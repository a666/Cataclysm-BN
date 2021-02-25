#include "language.h"

#if defined(LOCALIZE) && defined(__STRICT_ANSI__)
#  undef __STRICT_ANSI__ // _putenv in minGW need that
#  include <cstdlib>
#  define __STRICT_ANSI__
#endif

#if defined(LOCALIZE)
#  if defined(_WIN32)
#  if 1 // Prevent IWYU reordering platform_win.h below mmsystem.h
#    include "platform_win.h"
#  endif
#    include "mmsystem.h"
#  endif // _WIN32
#  if defined(MACOSX)
#    include <CoreFoundation/CFLocale.h>
#    include <CoreFoundation/CoreFoundation.h>
#  endif
#endif // LOCALIZE

#include "cached_options.h"
#include "debug.h"
#include "name.h"
#include "options.h"
#include "output.h"
#include "path_info.h"
#include "translations.h"
#if defined(LOCALIZE)
#  include "json.h"
#  include "ui_manager.h"
#endif

std::vector<language_info> lang_options = {
    // Note: language names are in their own language and are *not* translated at all.
    // Note: Somewhere in Github PR was better link to msdn.microsoft.com with language names.
    // http://en.wikipedia.org/wiki/List_of_language_names
    { "en", R"(English)", "en_US.UTF-8" },
#if defined(LOCALIZE)
    { "de", R"(Deutsch)", "de_DE.UTF-8" },
    { "es_AR", R"(Español (Argentina))", "es_AR.UTF-8" },
    { "es_ES", R"(Español (España))", "es_ES.UTF-8" },
    { "fr", R"(Français)", "fr_FR.UTF-8" },
    { "hu", R"(Magyar)", "hu_HU.UTF-8" },
    { "ja", R"(日本語)", "ja_JP.UTF-8" },
    { "ko", R"(한국어)", "ko_KR.UTF-8" },
    { "pl", R"(Polski)", "pl_PL.UTF-8" },
    { "pt_BR", R"(Português (Brasil))", "pt_BR.UTF-8" },
    { "ru", R"(Русский)", "ru_RU.UTF-8" },
    { "zh_CN", R"(中文 (天朝))", "zh_CN.UTF-8" },
    { "zh_TW", R"(中文 (台灣))", "zh_TW.UTF-8" },
#endif // LOCALIZE
};

static language_info const *get_lang_info( const std::string &lang )
{
    for( const language_info &li : lang_options ) {
        if( li.id == lang ) {
            return &li;
        }
    }
    // Should never happen
    debugmsg( "'%s' is not a valid language", lang );
    return &lang_options[0];
}

const std::vector<language_info> &list_available_languages()
{
    return lang_options;
}

// Names depend on the language settings. They are loaded from different files
// based on the currently used language. If that changes, we have to reload the
// names.
static void reload_names()
{
    Name::clear();
    Name::load_from_file( PATH_INFO::names() );
}

#if defined(LOCALIZE)
#if defined(MACOSX)
std::string getOSXSystemLang()
{
    // Get the user's language list (in order of preference)
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    if( CFArrayGetCount( langs ) == 0 ) {
        return "en_US";
    }

    CFStringRef lang = static_cast<CFStringRef>( CFArrayGetValueAtIndex( langs, 0 ) );
    const char *lang_code_raw_fast = CFStringGetCStringPtr( lang, kCFStringEncodingUTF8 );
    std::string lang_code;
    if( lang_code_raw_fast ) { // fast way, probably it's never works
        lang_code = lang_code_raw_fast;
    } else { // fallback to slow way
        CFIndex length = CFStringGetLength( lang ) + 1;
        std::vector<char> lang_code_raw_slow( length, '\0' );
        bool success = CFStringGetCString( lang, lang_code_raw_slow.data(), length, kCFStringEncodingUTF8 );
        if( !success ) {
            return "en_US";
        }
        lang_code = lang_code_raw_slow.data();
    }

    // Convert to the underscore format expected by gettext
    std::replace( lang_code.begin(), lang_code.end(), '-', '_' );

    /**
     * Handle special case for simplified/traditional Chinese. Simplified/Traditional
     * is actually denoted by the region code in older iterations of the
     * language codes, whereas now (at least on OS X) region is distinct.
     * That is, CDDA expects 'zh_CN' but OS X might give 'zh-Hans-CN'.
     */
    if( string_starts_with( lang_code, "zh_Hans" ) ) {
        return "zh_CN";
    } else if( string_starts_with( lang_code, "zh_Hant" ) ) {
        return "zh_TW";
    }

    return isValidLanguage( lang_code ) ? lang_code : "en_US";
}
#endif // MACOSX

bool isValidLanguage( const std::string &lang )
{
    const auto languages = get_options().get_option( "USE_LANG" ).getItems();
    return std::find_if( languages.begin(),
    languages.end(), [&lang]( const options_manager::id_and_option & pair ) {
        return pair.first == lang || pair.first == lang.substr( 0, pair.first.length() );
    } ) != languages.end();
}

/* "Useful" links:
 *  https://www.science.co.il/language/Locale-codes.php
 *  https://support.microsoft.com/de-de/help/193080/how-to-use-the-getuserdefaultlcid-windows-api-function-to-determine-op
 *  https://msdn.microsoft.com/en-us/library/cc233965.aspx
 */
std::string getLangFromLCID( const int &lcid )
{
    static std::map<std::string, std::set<int>> lang_lcid;
    if( lang_lcid.empty() ) {
        lang_lcid["en"] = {{ 1033, 2057, 3081, 4105, 5129, 6153, 7177, 8201, 9225, 10249, 11273 }};
        lang_lcid["fr"] = {{ 1036, 2060, 3084, 4108, 5132 }};
        lang_lcid["de"] = {{ 1031, 2055, 3079, 4103, 5127 }};
        lang_lcid["it_IT"] = {{ 1040, 2064 }};
        lang_lcid["es_AR"] = { 11274 };
        lang_lcid["es_ES"] = {{ 1034, 2058, 3082, 4106, 5130, 6154, 7178, 8202, 9226, 10250, 12298, 13322, 14346, 15370, 16394, 17418, 18442, 19466, 20490 }};
        lang_lcid["ja"] = { 1041 };
        lang_lcid["ko"] = { 1042 };
        lang_lcid["pl"] = { 1045 };
        lang_lcid["pt_BR"] = {{ 1046, 2070 }};
        lang_lcid["ru"] = { 1049 };
        lang_lcid["zh_CN"] = {{ 2052, 3076, 4100 }};
        lang_lcid["zh_TW"] = { 1028 };
    }

    for( auto &lang : lang_lcid ) {
        if( lang.second.find( lcid ) != lang.second.end() ) {
            return lang.first;
        }
    }
    return "";
}

void select_language()
{
    auto languages = get_options().get_option( "USE_LANG" ).getItems();

    languages.erase( std::remove_if( languages.begin(),
    languages.end(), []( const options_manager::id_and_option & lang ) {
        return lang.first.empty() || lang.second.empty();
    } ), languages.end() );

    uilist sm;
    sm.allow_cancel = false;
    sm.text = _( "Select your language" );
    for( size_t i = 0; i < languages.size(); i++ ) {
        sm.addentry( i, true, MENU_AUTOASSIGN, languages[i].second.translated() );
    }
    sm.query();

    get_options().get_option( "USE_LANG" ).setValue( languages[sm.ret].first );
    get_options().save();
}

void set_language()
{
    std::string win_or_mac_lang;
#if defined(_WIN32)
    win_or_mac_lang = getLangFromLCID( GetUserDefaultLCID() );
#endif
#if defined(MACOSX)
    win_or_mac_lang = getOSXSystemLang();
#endif
    // Step 1. Setup locale settings.
    std::string lang_opt = get_option<std::string>( "USE_LANG" ).empty() ? win_or_mac_lang :
                           get_option<std::string>( "USE_LANG" );
    if( !lang_opt.empty() ) {
        // Not 'System Language'
        // Overwrite all system locale settings. Use CDDA settings. User wants this.
#if defined(_WIN32)
        std::string lang_env = "LANGUAGE=" + lang_opt;
        if( _putenv( lang_env.c_str() ) != 0 ) {
            DebugLog( D_WARNING, D_MAIN ) << "Can't set 'LANGUAGE' environment variable";
        }
#else
        if( setenv( "LANGUAGE", lang_opt.c_str(), true ) != 0 ) {
            DebugLog( D_WARNING, D_MAIN ) << "Can't set 'LANGUAGE' environment variable";
        }
#endif
        else {
            const auto env = getenv( "LANGUAGE" );
            if( env != nullptr ) {
                DebugLog( D_INFO, D_MAIN ) << "[lang] Language is set to: '" << env << '\'';
            } else {
                DebugLog( D_WARNING, D_MAIN ) << "Can't get 'LANGUAGE' environment variable";
            }
        }
    }

#if defined(_WIN32)
    // Use the ANSI code page 1252 to work around some language output bugs.
    if( setlocale( LC_ALL, ".1252" ) == nullptr ) {
        DebugLog( D_WARNING, D_MAIN ) << "Error while setlocale(LC_ALL, '.1252').";
    }
    DebugLog( D_INFO, DC_ALL ) << "[lang] C locale set to " << setlocale( LC_ALL, nullptr );
    DebugLog( D_INFO, DC_ALL ) << "[lang] C++ locale set to " << std::locale().name();
#endif

    // Step 2. Bind to gettext domain.
    std::string locale_dir;
#if defined(__ANDROID__)
    // HACK: Since we're using libintl-lite instead of libintl on Android, we hack the locale_dir to point directly to the .mo file.
    // This is because of our hacky libintl-lite bindtextdomain() implementation.
    auto env = getenv( "LANGUAGE" );
    locale_dir = std::string( PATH_INFO::base_path() + "lang/mo/" + ( env ? env : "none" ) +
                              "/LC_MESSAGES/cataclysm-bn.mo" );
#elif (defined(__linux__) || (defined(MACOSX) && !defined(TILES)))
    if( !PATH_INFO::base_path().empty() ) {
        locale_dir = PATH_INFO::base_path() + "share/locale";
    } else {
        locale_dir = "lang/mo";
    }
#else
    locale_dir = "lang/mo";
#endif

    const char *locale_dir_char = locale_dir.c_str();
    bindtextdomain( "cataclysm-bn", locale_dir_char );
    bind_textdomain_codeset( "cataclysm-bn", "UTF-8" );
    textdomain( "cataclysm-bn" );

    // Step 3. Finalize
    invalidate_translations();
    reload_names();
}

#else // !LOCALIZE

#include <cstring> // strcmp
#include <map>

bool isValidLanguage( const std::string &/*lang*/ )
{
    return true;
}

std::string getLangFromLCID( const int &/*lcid*/ )
{
    return "";
}

void select_language()
{
    return;
}

void set_language()
{
    reload_names();
    return;
}

#endif // LOCALIZE

void update_global_locale()
{
    std::string lang = ::get_option<std::string>( "USE_LANG" );

    // TODO: reset to system locale when selecting 'System language'
    if( !lang.empty() ) {
        try {
            std::locale::global( std::locale( get_lang_info( lang )->locale ) );
        } catch( std::runtime_error &e ) {
            std::locale::global( std::locale() );
        }
    }

    DebugLog( D_INFO, DC_ALL ) << "[lang] C locale set to " << setlocale( LC_ALL, nullptr );
    DebugLog( D_INFO, DC_ALL ) << "[lang] C++ locale set to " << std::locale().name();
}

bool localized_comparator::operator()( const std::string &l, const std::string &r ) const
{
    // We need different implementations on each platform.  MacOS seems to not
    // support localized comparison of strings via the standard library at all,
    // so resort to MacOS-specific solution.  Windows cannot be expected to be
    // using a UTF-8 locale (whereas our strings are always UTF-8) and so we
    // must convert to wstring for comparison there.  Linux seems to work as
    // expected on regular strings; no workarounds needed.
    // See https://github.com/CleverRaven/Cataclysm-DDA/pull/40041 for further
    // discussion.
#if defined(MACOSX)
    CFStringRef lr = CFStringCreateWithCStringNoCopy( kCFAllocatorDefault, l.c_str(),
                     kCFStringEncodingUTF8, kCFAllocatorNull );
    CFStringRef rr = CFStringCreateWithCStringNoCopy( kCFAllocatorDefault, r.c_str(),
                     kCFStringEncodingUTF8, kCFAllocatorNull );
    bool result = CFStringCompare( lr, rr, kCFCompareLocalized ) < 0;
    CFRelease( lr );
    CFRelease( rr );
    return result;
#elif defined(_WIN32)
    return ( *this )( utf8_to_wstr( l ), utf8_to_wstr( r ) );
#else
    return std::locale()( l, r );
#endif
}

bool localized_comparator::operator()( const std::wstring &l, const std::wstring &r ) const
{
#if defined(MACOSX)
    return ( *this )( wstr_to_utf8( l ), wstr_to_utf8( r ) );
#else
    return std::locale()( l, r );
#endif
}
