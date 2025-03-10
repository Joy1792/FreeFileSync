// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "version_check.h"
#include <ctime>
#include <zen/crc.h>
#include <zen/string_tools.h>
#include <zen/i18n.h>
#include <zen/utf.h>
#include <zen/scope_guard.h>
#include <zen/build_info.h>
#include <zen/basic_math.h>
#include <zen/file_io.h>
#include <zen/file_error.h>
#include <zen/http.h>
#include <zen/process_exec.h>
#include <zen/sys_version.h>
#include <zen/sys_info.h>
#include <zen/thread.h>
#include <wx+/popup_dlg.h>
#include <wx+/image_resources.h>
#include "../ffs_paths.h"
#include "../version/version.h"
#include "small_dlgs.h"

    #include <zen/symlink_target.h>


using namespace zen;
using namespace fff;


namespace
{
const Zchar ffsUpdateCheckUserAgent[] = Zstr("FFS-Update-Check");


time_t getVersionCheckInactiveId()
{
    //use current version to calculate a changing number for the inactive state near UTC begin, in order to always check for updates after installing a new version
    //=> interpret version as 11-based *unique* number (this breaks lexicographical version ordering, but that's irrelevant!)
    int id = 0;
    const char* first = ffsVersion;
    const char* last = first + zen::strLength(ffsVersion);
    std::for_each(first, last, [&](char c)
    {
        id *= 11;
        if ('0' <= c && c <= '9')
            id += c - '0';
        else
        {
            assert(c == FFS_VERSION_SEPARATOR);
            id += 10;
        }
    });
    assert(0 < id && id < 3600 * 24 * 365); //as long as value is within a year after UTC begin (1970) there's no risk to clash with *current* time
    return id;
}




time_t getVersionCheckCurrentTime()
{
    time_t now = std::time(nullptr);
    return now;
}


void openBrowserForDownload(wxWindow* parent)
{
    wxLaunchDefaultBrowser(L"https://freefilesync.org/get_latest.php");
}
}


bool fff::shouldRunAutomaticUpdateCheck(time_t lastUpdateCheck)
{
    if (lastUpdateCheck == getVersionCheckInactiveId())
        return false;

    const time_t now = std::time(nullptr);
    return std::abs(now - lastUpdateCheck) >= 7 * 24 * 3600; //check weekly
}


std::wstring getIso639Language()
{
    assert(runningOnMainThread()); //this function is not thread-safe: consider wxWidgets usage

    std::wstring localeName(wxLocale::GetLanguageCanonicalName(wxLocale::GetSystemLanguage()));
    localeName = beforeFirst(localeName, L'@', IfNotFoundReturn::all); //the locale may contain an @, e.g. "sr_RS@latin"; see wxLocale::InitLanguagesDB()

    if (!localeName.empty())
    {
        const std::wstring langCode = beforeFirst(localeName, L'_', IfNotFoundReturn::all);
        assert(langCode.size() == 2 || langCode.size() == 3); //ISO 639: 3-letter possible!
        return langCode;
    }
    assert(false);
    return L"zz";
}


namespace
{
std::wstring getIso3166Country()
{
    assert(runningOnMainThread()); //this function is not thread-safe, consider wxWidgets usage

    std::wstring localeName(wxLocale::GetLanguageCanonicalName(wxLocale::GetSystemLanguage()));
    localeName = beforeFirst(localeName, L'@', IfNotFoundReturn::all); //the locale may contain an @, e.g. "sr_RS@latin"; see wxLocale::InitLanguagesDB()

    if (contains(localeName, L'_'))
    {
        const std::wstring cc = afterFirst(localeName, L'_', IfNotFoundReturn::none);
        assert(cc.size() == 2 || cc.size() == 3); //ISO 3166: 3-letter possible!
        return cc;
    }
    assert(false);
    return L"ZZ";
}


//coordinate with get_latest_version_number.php
std::vector<std::pair<std::string, std::string>> geHttpPostParameters(wxWindow& parent) //throw SysError
{
    assert(runningOnMainThread()); //this function is not thread-safe, e.g. consider wxWidgets usage in getIso639Language()
    std::vector<std::pair<std::string, std::string>> params;

    params.emplace_back("ffs_version", ffsVersion);


    params.emplace_back("os_name", "Linux");

    const OsVersion osv = getOsVersion();
    params.emplace_back("os_version", numberTo<std::string>(osv.major) + "." + numberTo<std::string>(osv.minor));

    const char* osArch = cpuArchName;
    params.emplace_back("os_arch", osArch);

#ifdef __WXGTK2__
    //wxWindow::GetContentScaleFactor() requires GTK3 or later
#elif defined __WXGTK3__
    params.emplace_back("dip_scale", numberTo<std::string>(parent.GetContentScaleFactor()));
#else
#error unknown GTK version!
#endif

    const std::string ffsLang = []
    {
        const wxLanguage lang = getLanguage();

        for (const TranslationInfo& ti : getAvailableTranslations())
            if (ti.languageID == lang)
                return ti.locale;
        return std::string("zz");
    }();
    params.emplace_back("ffs_lang",  ffsLang);

    params.emplace_back("language", utfTo<std::string>(getIso639Language()));
    params.emplace_back("country",  utfTo<std::string>(getIso3166Country()));

    return params;
}




void showUpdateAvailableDialog(wxWindow* parent, const std::string& onlineVersion)
{
    std::wstring updateDetailsMsg;
    try
    {
        updateDetailsMsg = utfTo<std::wstring>(sendHttpGet(utfTo<Zstring>("https://api.freefilesync.org/latest_changes?" + xWwwFormUrlEncode({{"since", ffsVersion}})),
        ffsUpdateCheckUserAgent, Zstring() /*caCertFilePath*/).readAll(nullptr /*notifyUnbufferedIO*/)); //throw SysError
    }
    catch (const SysError& e) { updateDetailsMsg = _("Failed to retrieve update information.") + + L"\n\n" + e.toString(); }


    switch (showConfirmationDialog(parent, DialogInfoType::info, PopupDialogCfg().
                                   setIcon(loadImage("FreeFileSync", fastFromDIP(48))).
                                   setTitle(_("Check for Program Updates")).
                                   setMainInstructions(replaceCpy(_("FreeFileSync %x is available!"), L"%x", utfTo<std::wstring>(onlineVersion)) + L"\n\n" + _("Download now?")).
                                   setDetailInstructions(updateDetailsMsg), _("&Download")))
    {
        case ConfirmationButton::accept: //download
            openBrowserForDownload(parent);
            break;
        case ConfirmationButton::cancel:
            break;
    }
}


std::string getOnlineVersion(const std::vector<std::pair<std::string, std::string>>& postParams) //throw SysError
{
    const std::string response = sendHttpPost(Zstr("https://api.freefilesync.org/latest_version"), postParams, nullptr /*notifyUnbufferedIO*/,
                                              ffsUpdateCheckUserAgent, Zstring() /*caCertFilePath*/).readAll(nullptr /*notifyUnbufferedIO*/); //throw SysError

    if (response.empty() ||
    !std::all_of(response.begin(), response.end(), [](char c) { return isDigit(c) || c == FFS_VERSION_SEPARATOR; }) ||
    startsWith(response, FFS_VERSION_SEPARATOR) ||
    endsWith(response, FFS_VERSION_SEPARATOR) ||
    contains(response, std::string() + FFS_VERSION_SEPARATOR + FFS_VERSION_SEPARATOR))
    throw SysError(L"Unexpected server response: \"" +  utfTo<std::wstring>(response) + L'"');
    //response may be "This website has been moved...", or a Javascript challenge: https://freefilesync.org/forum/viewtopic.php?t=8400

    return response;
}
}


bool fff::haveNewerVersionOnline(const std::string& onlineVersion)
{
    auto parseVersion = [](const std::string_view& version)
    {
        std::vector<size_t> output;
        split(version, FFS_VERSION_SEPARATOR,
        [&](const std::string_view digit) { output.push_back(stringTo<size_t>(digit)); });
        assert(!output.empty());
        return output;
    };
    const std::vector<size_t> current = parseVersion(ffsVersion);
    const std::vector<size_t> online  = parseVersion(onlineVersion);

    if (online.empty() || online[0] == 0) //online version string may be "Unknown", see automaticUpdateCheckEval() below!
        return true;

    return online > current; //std::vector compares lexicographically
}


bool fff::updateCheckActive(time_t lastUpdateCheck)
{
    return lastUpdateCheck != getVersionCheckInactiveId();
}


void fff::disableUpdateCheck(time_t& lastUpdateCheck)
{
    lastUpdateCheck = getVersionCheckInactiveId();
}


void fff::checkForUpdateNow(wxWindow& parent, std::string& lastOnlineVersion)
{
    try
    {
        const std::string onlineVersion = getOnlineVersion(geHttpPostParameters(parent)); //throw SysError
        lastOnlineVersion = onlineVersion;

        if (haveNewerVersionOnline(onlineVersion))
            showUpdateAvailableDialog(&parent, onlineVersion);
        else
            showNotificationDialog(&parent, DialogInfoType::info, PopupDialogCfg().
                                   setIcon(loadImage("update_check")).
                                   setTitle(_("Check for Program Updates")).
                                   setMainInstructions(_("FreeFileSync is up-to-date.")));
    }
    catch (const SysError& e)
    {
        if (internetIsAlive())
        {
            lastOnlineVersion = "Unknown";

            switch (showConfirmationDialog(&parent, DialogInfoType::error, PopupDialogCfg().
                                           setTitle(_("Check for Program Updates")).
                                           setMainInstructions(_("Cannot find current FreeFileSync version number online. A newer version is likely available. Check manually now?")).
                                           setDetailInstructions(e.toString()), _("&Check"), _("&Retry")))
            {
                case ConfirmationButton2::accept:
                    openBrowserForDownload(&parent);
                    break;
                case ConfirmationButton2::accept2: //retry
                    checkForUpdateNow(parent, lastOnlineVersion); //note: retry via recursion!!!
                    break;
                case ConfirmationButton2::cancel:
                    break;
            }
        }
        else
            switch (showConfirmationDialog(&parent, DialogInfoType::error, PopupDialogCfg().
                                           setTitle(_("Check for Program Updates")).
                                           setMainInstructions(replaceCpy(_("Unable to connect to %x."), L"%x", L"freefilesync.org")).
                                           setDetailInstructions(e.toString()), _("&Retry")))
            {
                case ConfirmationButton::accept: //retry
                    checkForUpdateNow(parent, lastOnlineVersion); //note: retry via recursion!!!
                    break;
                case ConfirmationButton::cancel:
                    break;
            }
    }
}


struct fff::UpdateCheckResultPrep
{
    std::vector<std::pair<std::string, std::string>> postParameters;
    std::optional<SysError> error;
};
std::shared_ptr<const UpdateCheckResultPrep> fff::automaticUpdateCheckPrepare(wxWindow& parent)
{
    assert(runningOnMainThread());
    auto prep = std::make_shared<UpdateCheckResultPrep>();
    try
    {
        prep->postParameters = geHttpPostParameters(parent); //throw SysError
    }
    catch (const SysError& e)
    {
        prep->error = e;
    }
    return prep;
}


struct fff::UpdateCheckResult
{
    std::string onlineVersion;
    bool internetIsAlive = false;
    std::optional<SysError> error;
};
std::shared_ptr<const UpdateCheckResult> fff::automaticUpdateCheckRunAsync(const UpdateCheckResultPrep* resultPrep)
{
    //assert(!runningOnMainThread()); -> allow synchronous call, too
    auto result = std::make_shared<UpdateCheckResult>();
    try
    {
        if (resultPrep->error)
            throw* resultPrep->error; //throw SysError

        result->onlineVersion = getOnlineVersion(resultPrep->postParameters); //throw SysError
        result->internetIsAlive = true;
    }
    catch (const SysError& e)
    {
        result->error = e;
        result->internetIsAlive = internetIsAlive();
    }
    return result;
}


void fff::automaticUpdateCheckEval(wxWindow& parent, time_t& lastUpdateCheck, std::string& lastOnlineVersion, const UpdateCheckResult* asyncResult)
{
    assert(runningOnMainThread());

    const UpdateCheckResult& result = *asyncResult;

    if (!result.error)
    {
        lastUpdateCheck   = getVersionCheckCurrentTime();
        lastOnlineVersion = result.onlineVersion;

        if (haveNewerVersionOnline(result.onlineVersion))
            showUpdateAvailableDialog(&parent, result.onlineVersion);
    }
    else
    {
        if (result.internetIsAlive)
        {
            lastOnlineVersion = "Unknown";

            switch (showConfirmationDialog(&parent, DialogInfoType::error, PopupDialogCfg().
                                           setTitle(_("Check for Program Updates")).
                                           setMainInstructions(_("Cannot find current FreeFileSync version number online. A newer version is likely available. Check manually now?")).
                                           setDetailInstructions(result.error->toString()),
                                           _("&Check"), _("&Retry")))
            {
                case ConfirmationButton2::accept:
                    openBrowserForDownload(&parent);
                    break;
                case ConfirmationButton2::accept2: //retry
                    automaticUpdateCheckEval(parent, lastUpdateCheck, lastOnlineVersion,
                                             automaticUpdateCheckRunAsync(automaticUpdateCheckPrepare(parent).get()).get()); //note: retry via recursion!!!
                    break;
                case ConfirmationButton2::cancel:
                    break;
            }
        }
        //else: ignore this error
    }
}


