// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "batch_status_handler.h"
#include <zen/shutdown.h>
#include <zen/resolve_path.h>
#include <wx+/popup_dlg.h>
#include <wx/app.h>
#include <wx/sound.h>
#include "../afs/concrete.h"
#include "../config.h"
//#include "../log_file.h"

using namespace zen;
using namespace fff;


BatchStatusHandler::BatchStatusHandler(bool showProgress,
                                       const std::wstring& jobName,
                                       const std::chrono::system_clock::time_point& startTime,
                                       bool ignoreErrors,
                                       size_t autoRetryCount,
                                       std::chrono::seconds autoRetryDelay,
                                       const Zstring& soundFileSyncComplete,
                                       const Zstring& soundFileAlertPending,
                                       const std::optional<wxSize>& progressDlgSize, bool dlgMaximize,
                                       bool autoCloseDialog,
                                       PostSyncAction postSyncAction,
                                       BatchErrorHandling batchErrorHandling) :
    jobName_(jobName),
    startTime_(startTime),
    autoRetryCount_(autoRetryCount),
    autoRetryDelay_(autoRetryDelay),
    soundFileSyncComplete_(soundFileSyncComplete),
    soundFileAlertPending_(soundFileAlertPending),
    progressDlg_(SyncProgressDialog::create(progressDlgSize, dlgMaximize, [this] { userRequestAbort(); }, *this, nullptr /*parentWindow*/, showProgress, autoCloseDialog,
{jobName}, std::chrono::system_clock::to_time_t(startTime), ignoreErrors, autoRetryCount, [&]
{
    switch (postSyncAction)
    {
        case PostSyncAction::none:
            return PostSyncAction2::none;
        case PostSyncAction::sleep:
            return PostSyncAction2::sleep;
        case PostSyncAction::shutdown:
            return PostSyncAction2::shutdown;
    }
    assert(false);
    return PostSyncAction2::none;
}())),
batchErrorHandling_(batchErrorHandling)
{
    //ATTENTION: "progressDlg_" is an unmanaged resource!!! However, at this point we already consider construction complete! =>
    //ZEN_ON_SCOPE_FAIL( cleanup(); ); //destructor call would lead to member double clean-up!!!
}


BatchStatusHandler::~BatchStatusHandler()
{
    if (progressDlg_) //reportResults() was not called!
        std::abort();
}


BatchStatusHandler::Result BatchStatusHandler::reportResults(const Zstring& postSyncCommand, PostSyncCondition postSyncCondition,
                                                             const AbstractPath& logFolderPath, int logfilesMaxAgeDays, LogFileFormat logFormat,
                                                             const std::set<AbstractPath>& logFilePathsToKeep,
                                                             const std::string& emailNotifyAddress, ResultsNotification emailNotifyCondition) //noexcept!!
{
    //keep correct summary window stats considering count down timer, system sleep
    const std::chrono::milliseconds totalTime = progressDlg_->pauseAndGetTotalTime();

    //determine post-sync status irrespective of further errors during tear-down
    const SyncResult syncResult = [&]
    {
        if (getAbortStatus())
        {
            logMsg(errorLog_, _("Stopped"), MSG_TYPE_ERROR); //= user cancel
            return SyncResult::aborted;
        }
        const ErrorLogStats logCount = getStats(errorLog_);
        if (logCount.error > 0)
            return SyncResult::finishedError;
        else if (logCount.warning > 0)
            return SyncResult::finishedWarning;

        if (getStatsTotal() == ProgressStats())
            logMsg(errorLog_, _("Nothing to synchronize"), MSG_TYPE_INFO);
        return SyncResult::finishedSuccess;
    }();

    assert(syncResult == SyncResult::aborted || currentPhase() == ProcessPhase::synchronizing);

    const ProcessSummary summary
    {
        startTime_, syncResult, {jobName_},
        getStatsCurrent(),
        getStatsTotal  (),
        totalTime
    };

    AbstractPath logFilePath = AFS::appendRelPath(logFolderPath, generateLogFileName(logFormat, summary));
    //e.g. %AppData%\FreeFileSync\Logs\Backup FreeFileSync 2013-09-15 015052.123 [Error].log

    auto notifyStatusNoThrow = [&](std::wstring&& msg) { try { updateStatus(std::move(msg)); /*throw AbortProcess*/ } catch (AbortProcess&) {} };

    bool autoClose = false;
    FinalRequest finalRequest = FinalRequest::none;
    bool suspend = false;

    if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
        ; /* user cancelled => don't run post sync command
                            => don't send email notification
                            => don't run post sync action
                            => don't play sound notification   */
    else
    {
        //--------------------- post sync command ----------------------
        if (const Zstring cmdLine = trimCpy(postSyncCommand);
            !cmdLine.empty())
            if (postSyncCondition == PostSyncCondition::completion ||
                (postSyncCondition == PostSyncCondition::errors) == (syncResult == SyncResult::aborted ||
                                                                     syncResult == SyncResult::finishedError))
                ////----------------------------------------------------------------------
                //::wxSetEnv(L"logfile_path", AFS::getDisplayPath(logFilePath));
                ////----------------------------------------------------------------------
                runCommandAndLogErrors(expandMacros(cmdLine), errorLog_);

        //--------------------- email notification ----------------------
        if (const std::string notifyEmail = trimCpy(emailNotifyAddress);
            !notifyEmail.empty())
            if (emailNotifyCondition == ResultsNotification::always ||
                (emailNotifyCondition == ResultsNotification::errorWarning && (syncResult == SyncResult::aborted       ||
                                                                               syncResult == SyncResult::finishedError ||
                                                                               syncResult == SyncResult::finishedWarning)) ||
                (emailNotifyCondition == ResultsNotification::errorOnly && (syncResult == SyncResult::aborted ||
                                                                            syncResult == SyncResult::finishedError)))
                try
                {
                    logMsg(errorLog_, replaceCpy(_("Sending email notification to %x"), L"%x", utfTo<std::wstring>(notifyEmail)), MSG_TYPE_INFO);
                    sendLogAsEmail(notifyEmail, summary, errorLog_, logFilePath, notifyStatusNoThrow); //throw FileError
                }
                catch (const FileError& e) { logMsg(errorLog_, e.toString(), MSG_TYPE_ERROR); }

        //--------------------- post sync actions ----------------------
        auto proceedWithShutdown = [&](const std::wstring& operationName)
        {
            if (progressDlg_->getWindowIfVisible())
                try
                {
                    assert(!zen::endsWith(operationName, L"."));
                    auto notifyStatusThrowOnCancel = [&](const std::wstring& timeRemMsg)
                    {
                        try { updateStatus(operationName + L"... " + timeRemMsg); /*throw AbortProcess*/ }
                        catch (AbortProcess&)
                        {
                            if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
                                throw;
                        }
                    };
                    delayAndCountDown(std::chrono::steady_clock::now() + std::chrono::seconds(10), notifyStatusThrowOnCancel); //throw AbortProcess
                }
                catch (AbortProcess&) { return false; }

            return true;
        };

        switch (progressDlg_->getOptionPostSyncAction())
        {
            case PostSyncAction2::none:
                autoClose = progressDlg_->getOptionAutoCloseDialog();
                break;
            case PostSyncAction2::exit:
                assert(false);
                break;
            case PostSyncAction2::sleep:
                if (proceedWithShutdown(_("System: Sleep")))
                {
                    autoClose = progressDlg_->getOptionAutoCloseDialog();
                    suspend = true;
                }
                break;
            case PostSyncAction2::shutdown:
                if (proceedWithShutdown(_("System: Shut down")))
                {
                    autoClose = true;
                    finalRequest = FinalRequest::shutdown; //system shutdown must be handled by calling context!
                }
                break;
        }

        //--------------------- sound notification ----------------------
        if (!autoClose) //only play when showing results dialog
            if (!soundFileSyncComplete_.empty())
            {
                //wxWidgets shows modal error dialog by default => "no, wxWidgets, NO!"
                wxLog* oldLogTarget = wxLog::SetActiveTarget(new wxLogStderr); //transfer and receive ownership!
                ZEN_ON_SCOPE_EXIT(delete wxLog::SetActiveTarget(oldLogTarget));

                wxSound::Play(utfTo<wxString>(soundFileSyncComplete_), wxSOUND_ASYNC);
            }
        //if (::GetForegroundWindow() != GetHWND())
        //  RequestUserAttention(); -> probably too much since task bar is already colorized with Taskbar::STATUS_ERROR or STATUS_NORMAL
    }

    //--------------------- save log file ----------------------
    try //create not before destruction: 1. avoid issues with FFS trying to sync open log file 2. include status in log file name without extra rename
    {
        //do NOT use tryReportingError()! saving log files should not be cancellable!
        saveLogFile(logFilePath, summary, errorLog_, logfilesMaxAgeDays, logFormat, logFilePathsToKeep, notifyStatusNoThrow); //throw FileError
    }
    catch (const FileError& e)
    {
        logMsg(errorLog_, e.toString(), MSG_TYPE_ERROR);

        const AbstractPath logFileDefaultPath = AFS::appendRelPath(createAbstractPath(getLogFolderDefaultPath()), generateLogFileName(logFormat, summary));
        if (logFilePath != logFileDefaultPath) //fallback: log file *must* be saved no matter what!
            try
            {
                logFilePath = logFileDefaultPath;
                saveLogFile(logFileDefaultPath, summary, errorLog_, logfilesMaxAgeDays, logFormat, logFilePathsToKeep, notifyStatusNoThrow); //throw FileError
            }
            catch (const FileError& e2) { logMsg(errorLog_, e2.toString(), MSG_TYPE_ERROR); }
    }
    //----------------------------------------------------------

    if (suspend) //...*before* results dialog is shown
        try
        {
            suspendSystem(); //throw FileError
        }
        catch (const FileError& e) { logMsg(errorLog_, e.toString(), MSG_TYPE_ERROR); }

    if (switchToGuiRequested_) //-> avoid recursive yield() calls, thous switch not before ending batch mode
    {
        autoClose = true;
        finalRequest = FinalRequest::switchGui;
    }

    auto errorLogFinal = makeSharedRef<const ErrorLog>(std::move(errorLog_));

    const auto [autoCloseDialog, dlgSize, dlgIsMaximized] = progressDlg_->destroy(autoClose,
                                                                                  true /*restoreParentFrame: n/a here*/,
                                                                                  syncResult, errorLogFinal);
    progressDlg_ = nullptr;

    return {syncResult, getStats(errorLogFinal.ref()), finalRequest, logFilePath, dlgSize, dlgIsMaximized};
}


wxWindow* BatchStatusHandler::getWindowIfVisible()
{
    return progressDlg_ ? progressDlg_->getWindowIfVisible() : nullptr;
}


void BatchStatusHandler::initNewPhase(int itemsTotal, int64_t bytesTotal, ProcessPhase phaseID)
{
    StatusHandler::initNewPhase(itemsTotal, bytesTotal, phaseID);
    progressDlg_->initNewPhase(); //call after "StatusHandler::initNewPhase"

    //macOS needs a full yield to update GUI and get rid of "dummy" texts
    requestUiUpdate(true /*force*/); //throw AbortProcess
}


void BatchStatusHandler::updateDataProcessed(int itemsDelta, int64_t bytesDelta) //noexcept!
{
    StatusHandler::updateDataProcessed(itemsDelta, bytesDelta);

    //note: this method should NOT throw in order to properly allow undoing setting of statistics!
    progressDlg_->notifyProgressChange(); //noexcept
    //for "curveDataBytes_->addRecord()"
}


void BatchStatusHandler::logMessage(const std::wstring& msg, MsgType type)
{
    logMsg(errorLog_, msg, [&]
    {
        switch (type)
        {
            //*INDENT-OFF*
            case MsgType::info:    return MSG_TYPE_INFO;
            case MsgType::warning: return MSG_TYPE_WARNING;
            case MsgType::error:   return MSG_TYPE_ERROR;
            //*INDENT-ON*
        }
        assert(false);
        return MSG_TYPE_ERROR;
    }());
    requestUiUpdate(false /*force*/); //throw AbortProcess
}


void BatchStatusHandler::reportWarning(const std::wstring& msg, bool& warningActive)
{
    PauseTimers dummy(*progressDlg_);

    logMsg(errorLog_, msg, MSG_TYPE_WARNING);

    if (!warningActive)
        return;

    if (!progressDlg_->getOptionIgnoreErrors())
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                bool dontWarnAgain = false;
                switch (showQuestionDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::warning,
                                           PopupDialogCfg().setDetailInstructions(msg + L"\n\n" + _("You can switch to FreeFileSync's main window to resolve this issue.")).
                                           alertWhenPending(soundFileAlertPending_).
                                           setCheckBox(dontWarnAgain, _("&Don't show this warning again"), static_cast<ConfirmationButton3>(QuestionButton2::no)),
                                           _("&Ignore"), _("&Switch")))
                {
                    case QuestionButton2::yes: //ignore
                        warningActive = !dontWarnAgain;
                        break;

                    case QuestionButton2::no: //switch
                        logMsg(errorLog_, _("Switching to FreeFileSync's main window"), MSG_TYPE_INFO);
                        switchToGuiRequested_ = true; //treat as a special kind of cancel
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess

                    case QuestionButton2::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break; //keep it! last switch might not find match

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
}


ProcessCallback::Response BatchStatusHandler::reportError(const ErrorInfo& errorInfo)
{
    PauseTimers dummy(*progressDlg_);

    //log actual fail time (not "now"!)
    const time_t failTime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() -
                                                                 std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::steady_clock::now() - errorInfo.failTime));
    //auto-retry
    if (errorInfo.retryNumber < autoRetryCount_)
    {
        logMsg(errorLog_, errorInfo.msg + L"\n-> " + _("Automatic retry"), MSG_TYPE_INFO, failTime);
        delayAndCountDown(errorInfo.failTime + autoRetryDelay_,
                          [&, statusPrefix  = _("Automatic retry") +
                                              (errorInfo.retryNumber == 0 ? L"" : L' ' + formatNumber(errorInfo.retryNumber + 1)) + SPACED_DASH,
                              statusPostfix = SPACED_DASH + _("Error") + L": " + replaceCpy(errorInfo.msg, L'\n', L' ')](const std::wstring& timeRemMsg)
        { this->updateStatus(statusPrefix + timeRemMsg + statusPostfix); }); //throw AbortProcess
        return ProcessCallback::retry;
    }

    //always, except for "retry":
    auto guardWriteLog = makeGuard<ScopeGuardRunMode::onExit>([&] { logMsg(errorLog_, errorInfo.msg, MSG_TYPE_ERROR, failTime); });

    if (!progressDlg_->getOptionIgnoreErrors())
    {
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                               PopupDialogCfg().setDetailInstructions(errorInfo.msg).
                                               alertWhenPending(soundFileAlertPending_),
                                               _("&Ignore"), _("Ignore &all"), _("&Retry")))
                {
                    case ConfirmationButton3::accept: //ignore
                        return ProcessCallback::ignore;

                    case ConfirmationButton3::accept2: //ignore all
                        progressDlg_->setOptionIgnoreErrors(true);
                        return ProcessCallback::ignore;

                    case ConfirmationButton3::decline: //retry
                        guardWriteLog.dismiss();
                        logMsg(errorLog_, errorInfo.msg + L"\n-> " + _("Retrying operation..."), MSG_TYPE_INFO, failTime);
                        return ProcessCallback::retry;

                    case ConfirmationButton3::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break; //used if last switch didn't find a match

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
    }
    else
        return ProcessCallback::ignore;

    assert(false);
    return ProcessCallback::ignore; //dummy value
}


void BatchStatusHandler::reportFatalError(const std::wstring& msg)
{
    PauseTimers dummy(*progressDlg_);

    logMsg(errorLog_, msg, MSG_TYPE_ERROR);

    if (!progressDlg_->getOptionIgnoreErrors())
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                               PopupDialogCfg().setDetailInstructions(msg).
                                               alertWhenPending(soundFileAlertPending_),
                                               _("&Ignore"), _("Ignore &all")))
                {
                    case ConfirmationButton2::accept: //ignore
                        break;

                    case ConfirmationButton2::accept2: //ignore all
                        progressDlg_->setOptionIgnoreErrors(true);
                        break;

                    case ConfirmationButton2::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break;

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
}


void BatchStatusHandler::forceUiUpdateNoThrow()
{
    progressDlg_->updateGui();
}
