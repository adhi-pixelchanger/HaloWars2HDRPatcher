// Gui.cpp - the simple dialog and the advanced panel.

#include "Launcher.h"
#include "StatusIcon.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <cstdlib>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(                                                                   \
    linker,                                                                        \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

using namespace launcher;

namespace {

constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;

HINSTANCE g_instance = nullptr;
HWND g_sinkWindow = nullptr;
bool g_busy = false;
ui::Status g_status = ui::Status::Idle;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

void SetText(HWND dialog, int id, const std::wstring& text) {
    SetDlgItemTextW(dialog, id, text.c_str());
}

std::wstring GetText(HWND dialog, int id) {
    const HWND control = GetDlgItem(dialog, id);
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return L"";
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

bool IsChecked(HWND dialog, int id) {
    return IsDlgButtonChecked(dialog, id) == BST_CHECKED;
}

void Check(HWND dialog, int id, bool on) {
    CheckDlgButton(dialog, id, on ? BST_CHECKED : BST_UNCHECKED);
}

void FillStrategies(HWND dialog, int id) {
    for (Strategy strategy : kStrategies) {
        SendDlgItemMessageW(dialog, id, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(StrategyName(strategy)));
    }
}

Strategy SelectedStrategy(HWND dialog, int id) {
    const LRESULT index = SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0);
    return (index >= 0 && index < static_cast<LRESULT>(_countof(kStrategies)))
               ? kStrategies[index]
               : kDefaultStrategy;
}

void LogSink(const wchar_t* line, void* /*user*/) {
    if (g_sinkWindow == nullptr) return;
    PostMessageW(g_sinkWindow, WM_APP_LOG, 0,
                 reinterpret_cast<LPARAM>(_wcsdup(line == nullptr ? L"" : line)));
}

// ---------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------

struct Job {
    RunOptions options;
};

DWORD WINAPI WorkerThread(LPVOID parameter) {
    auto* job = static_cast<Job*>(parameter);
    const RunResult result = Run(job->options);
    delete job;

    PostMessageW(g_sinkWindow, WM_APP_DONE, result.injected ? 1 : 0,
                 reinterpret_cast<LPARAM>(_wcsdup(result.verdict.c_str())));
    return 0;
}

bool StartJob(HWND owner, const RunOptions& options) {
    if (g_busy) return false;

    g_sinkWindow = owner;
    SetLogSink(LogSink, nullptr);
    g_busy = true;
    g_status = ui::Status::Busy;

    auto* job = new Job{options};
    const HANDLE thread = CreateThread(nullptr, 0, WorkerThread, job, 0, nullptr);
    if (thread == nullptr) {
        delete job;
        g_busy = false;
        g_status = ui::Status::Failure;
        return false;
    }
    CloseHandle(thread);
    return true;
}

//////////////////////////////////////////////////////////////////////////////
// Advanced panel
//////////////////////////////////////////////////////////////////////////////

std::vector<std::wstring> g_packages;

constexpr int kModeItemHdr = 0;
constexpr int kModeItemSdr = 1;

HooksConfig ReadHooksBoxes(HWND dialog) {
    HooksConfig config;

    const int mode = static_cast<int>(
        SendDlgItemMessageW(dialog, IDC_MODE, CB_GETCURSEL, 0, 0));
    config.mode = (mode == kModeItemSdr) ? HookMode::Sdr : HookMode::Hdr;

    config.gateFilter = GetText(dialog, IDC_GATEFILTER);

    config.log = IsChecked(dialog, IDC_HOOKSLOG);

    config.logToFile = config.log && IsChecked(dialog, IDC_LOGTOFILE);
    return config;
}

void WriteHooksBoxes(HWND dialog, const HooksConfig& config) {
    SendDlgItemMessageW(dialog, IDC_MODE, CB_SETCURSEL,
                        (config.mode == HookMode::Sdr) ? kModeItemSdr : kModeItemHdr,
                        0);
    SetText(dialog, IDC_GATEFILTER, config.gateFilter);
    Check(dialog, IDC_HOOKSLOG, config.log);
    Check(dialog, IDC_LOGTOFILE, config.logToFile);

    EnableWindow(GetDlgItem(dialog, IDC_GATEFILTER),
                 (config.mode == HookMode::Hdr) ? TRUE : FALSE);

    EnableWindow(GetDlgItem(dialog, IDC_LOGTOFILE), config.log ? TRUE : FALSE);
}

RunOptions CollectTestingOptions(HWND dialog, bool useEnvironmentConfig) {
    RunOptions options;
    options.packageNeedle = GetText(dialog, IDC_PACKAGE);
    options.aumidOverride = GetText(dialog, IDC_AUMID);
    options.dllPath = GetText(dialog, IDC_DLLPATH);

    options.hooks = ReadHooksBoxes(dialog);
    options.useEnvironmentConfig = useEnvironmentConfig;
    return options;
}

void AppendLog(HWND dialog, const wchar_t* line) {
    const HWND log = GetDlgItem(dialog, IDC_LOG);
    if (log == nullptr) return;

    const int end = GetWindowTextLengthW(log);
    SendMessageW(log, EM_SETSEL, end, end);
    std::wstring text(line);
    text += L"\r\n";
    SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

void SetTestingBusy(HWND dialog, bool busy) {
    const int ids[] = {IDC_LAUNCH,  IDC_LAUNCHENV, IDC_SIMPLE,
                       IDC_REFRESH, IDC_RESOLVE,   IDC_BROWSE, IDC_DEFAULTS};
    for (int id : ids) EnableWindow(GetDlgItem(dialog, id), !busy);
}

void RefreshPackages(HWND dialog) {
    const std::wstring current = GetText(dialog, IDC_PACKAGE);
    const HWND combo = GetDlgItem(dialog, IDC_PACKAGE);

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_packages = EnumerateLaunchablePackages();
    for (const auto& name : g_packages) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    SetText(dialog, IDC_PACKAGE, current);
}

void ResolveTargetFields(HWND dialog) {
    const std::wstring needle = GetText(dialog, IDC_PACKAGE);
    if (needle.empty()) return;

    const std::wstring fullName = FindPackageFullName(needle);
    if (fullName.empty()) {
        SetText(dialog, IDC_FULLNAME, L"(no match)");
        SetText(dialog, IDC_AUMID, L"");
        return;
    }
    SetText(dialog, IDC_FULLNAME, fullName);
    SetText(dialog, IDC_AUMID, GetFirstAumid(fullName));
}

void BrowseForDll(HWND dialog) {
    wchar_t path[MAX_PATH] = {};
    wcscpy_s(path, GetText(dialog, IDC_DLLPATH).c_str());

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialog;
    ofn.lpstrFilter = L"DLL files\0*.dll\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) SetText(dialog, IDC_DLLPATH, path);
}

void CopyReport(HWND dialog) {
    const std::wstring report =
        GetText(dialog, IDC_VERDICT) + L"\r\n\r\n" + GetText(dialog, IDC_LOG);
    if (!OpenClipboard(dialog)) return;
    EmptyClipboard();

    const size_t bytes = (report.size() + 1) * sizeof(wchar_t);
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* target = GlobalLock(memory);
        if (target != nullptr) {
            memcpy(target, report.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        }
    }
    CloseClipboard();
}

void SyncStrategyToPath(HWND dialog) {
    Strategy strategy = kDefaultStrategy;
    const bool known = StrategyOfPath(GetText(dialog, IDC_DLLPATH), &strategy);
    SendDlgItemMessageW(dialog, IDC_STRATEGY, CB_SETCURSEL,
                        known ? static_cast<WPARAM>(strategy) : static_cast<WPARAM>(-1),
                        0);
}

void ApplyDefaults(HWND dialog) {
    const RunOptions defaults = DefaultRunOptions();

    SetText(dialog, IDC_PACKAGE, defaults.packageNeedle);
    SetText(dialog, IDC_DLLPATH, defaults.dllPath);
    SyncStrategyToPath(dialog);
    WriteHooksBoxes(dialog, defaults.hooks);
    ResolveTargetFields(dialog);
}

INT_PTR CALLBACK TestingProc(HWND dialog, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: {
            const HFONT mono = CreateFontW(
                -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            if (mono != nullptr) {
                SendDlgItemMessageW(dialog, IDC_LOG, WM_SETFONT,
                                    reinterpret_cast<WPARAM>(mono), TRUE);
            }
            for (const wchar_t* item : {L"HDR - only while a video plays",
                                        L"SDR - the whole session"}) {
                SendDlgItemMessageW(dialog, IDC_MODE, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(item));
            }
            FillStrategies(dialog, IDC_STRATEGY);

            RefreshPackages(dialog);
            ApplyDefaults(dialog);

            SetDlgItemTextW(
                dialog, IDC_VERDICT,
                L"Launch & Inject reports the target's process mitigation policies "
                L"before it injects, so a refusal names itself rather than leaving "
                L"the payload to be blamed for it.");
            return TRUE;
        }

        case WM_APP_LOG: {
            auto* line = reinterpret_cast<wchar_t*>(lParam);
            AppendLog(dialog, line);
            free(line);
            return TRUE;
        }

        case WM_APP_DONE: {
            auto* verdict = reinterpret_cast<wchar_t*>(lParam);
            SetDlgItemTextW(dialog, IDC_VERDICT, verdict);
            free(verdict);
            g_busy = false;
            g_status = (wParam != 0) ? ui::Status::Success : ui::Status::Failure;
            SetTestingBusy(dialog, false);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_REFRESH:  RefreshPackages(dialog); return TRUE;
                case IDC_RESOLVE:  ResolveTargetFields(dialog); return TRUE;
                case IDC_DEFAULTS: ApplyDefaults(dialog); return TRUE;
                case IDC_BROWSE:   BrowseForDll(dialog); return TRUE;
                case IDC_COPY:     CopyReport(dialog); return TRUE;

                case IDC_PACKAGE:
                    if (HIWORD(wParam) == CBN_SELENDOK) {
                        PostMessageW(dialog, WM_COMMAND, IDC_RESOLVE, 0);
                    }
                    return TRUE;

                case IDC_MODE:
                    if (HIWORD(wParam) == CBN_SELENDOK) {
                        WriteHooksBoxes(dialog, ReadHooksBoxes(dialog));
                    }
                    return TRUE;

                case IDC_STRATEGY:
                    if (HIWORD(wParam) == CBN_SELENDOK) {
                        SetText(dialog, IDC_DLLPATH,
                                StrategyDllPath(SelectedStrategy(dialog, IDC_STRATEGY)));
                    }
                    return TRUE;

                case IDC_DLLPATH:
                    if (HIWORD(wParam) == EN_CHANGE) SyncStrategyToPath(dialog);
                    return TRUE;

                case IDC_HOOKSLOG:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        WriteHooksBoxes(dialog, ReadHooksBoxes(dialog));
                    }
                    return TRUE;

                case IDC_SIMPLE:
                    PostMessageW(GetParent(dialog), WM_COMMAND, IDC_TESTING, 0);
                    return TRUE;

                case IDC_LAUNCH:
                case IDC_LAUNCHENV: {
                    const bool useEnv = LOWORD(wParam) == IDC_LAUNCHENV;
                    SetDlgItemTextW(dialog, IDC_LOG, L"");
                    SetDlgItemTextW(dialog, IDC_VERDICT, L"Working...");
                    SetTestingBusy(dialog, true);
                    if (!StartJob(dialog, CollectTestingOptions(dialog, useEnv))) {
                        SetTestingBusy(dialog, false);
                        SetDlgItemTextW(dialog, IDC_VERDICT,
                                        L"A run is already in progress.");
                    }
                    return TRUE;
                }

                default:
                    break;
            }
            return FALSE;

        default:
            break;
    }
    return FALSE;
}

//////////////////////////////////////////////////////////////////////////////
// Simple view
//////////////////////////////////////////////////////////////////////////////

std::wstring g_lastDetail;
HWND g_testingPanel = nullptr;
bool g_testingVisible = false;

constexpr int kSimpleControls[] = {
    IDC_HEADLINE, IDC_MODELABEL, IDC_MAINMODE, IDC_STRATEGYLABEL, IDC_MAINSTRATEGY,
    IDC_STATUSICON, IDC_STATUSTEXT, IDC_STATUSDETAIL, IDC_GO, IDC_TESTING};

constexpr int kSimpleClientWidth = 200;
constexpr int kSimpleClientHeight = 216;

constexpr int kPanelTop = 4;
constexpr int kBottomMargin = 8;

SIZE DialogUnits(HWND dialog) {
    RECT rect = {0, 0, 4, 8};
    MapDialogRect(dialog, &rect);
    return SIZE{rect.right, rect.bottom};
}

SIZE PanelSize() {
    RECT rect = {};
    if (g_testingPanel == nullptr || !GetWindowRect(g_testingPanel, &rect)) {
        return SIZE{0, 0};
    }
    return SIZE{rect.right - rect.left, rect.bottom - rect.top};
}

void LayoutView(HWND dialog, bool testing) {
    const SIZE unit = DialogUnits(dialog);
    auto x = [&](int du) { return MulDiv(du, unit.cx, 4); };
    auto y = [&](int du) { return MulDiv(du, unit.cy, 8); };

    for (int id : kSimpleControls) {
        ShowWindow(GetDlgItem(dialog, id), testing ? SW_HIDE : SW_SHOW);
    }

    const SIZE panel = PanelSize();

    if (g_testingPanel != nullptr) {
        SetWindowPos(g_testingPanel, HWND_TOP, 0, y(kPanelTop), panel.cx, panel.cy,
                     testing ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    }

    RECT client = {0, 0,
                   testing ? panel.cx : x(kSimpleClientWidth),
                   testing ? y(kPanelTop) + panel.cy + y(kBottomMargin)
                           : y(kSimpleClientHeight)};
    AdjustWindowRectEx(&client, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
                       FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top, SWP_NOMOVE | SWP_NOZORDER);

    g_testingVisible = testing;
}

HookMode SelectedMainMode(HWND dialog) {
    return SendDlgItemMessageW(dialog, IDC_MAINMODE, CB_GETCURSEL, 0, 0) == kModeItemSdr
               ? HookMode::Sdr
               : HookMode::Hdr;
}

void SetSimpleStatus(HWND dialog, ui::Status status, const std::wstring& text,
                     const std::wstring& detail) {
    g_status = status;
    SetText(dialog, IDC_STATUSTEXT, text);
    SetText(dialog, IDC_STATUSDETAIL, detail);
    InvalidateRect(GetDlgItem(dialog, IDC_STATUSICON), nullptr, TRUE);
}

INT_PTR CALLBACK SimpleProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: {
            HFONT headline = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                         FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            if (headline != nullptr) {
                SendDlgItemMessageW(dialog, IDC_HEADLINE, WM_SETFONT,
                                    reinterpret_cast<WPARAM>(headline), TRUE);
            }
            for (const wchar_t* item : {L"HDR", L"SDR"}) {
                SendDlgItemMessageW(dialog, IDC_MAINMODE, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(item));
            }
            SendDlgItemMessageW(dialog, IDC_MAINMODE, CB_SETCURSEL,
                                DefaultRunOptions().hooks.mode == HookMode::Sdr
                                    ? kModeItemSdr
                                    : kModeItemHdr,
                                0);
            FillStrategies(dialog, IDC_MAINSTRATEGY);
            SendDlgItemMessageW(dialog, IDC_MAINSTRATEGY, CB_SETCURSEL,
                                static_cast<WPARAM>(kDefaultStrategy), 0);

            g_testingPanel = CreateDialogParamW(
                g_instance, MAKEINTRESOURCEW(IDD_TESTING), dialog, TestingProc, 0);
            LayoutView(dialog, false);

            if (IsProcessElevated()) {
                SetSimpleStatus(dialog, ui::Status::Warning, L"Running elevated.",
                                L"Windows will not launch Store apps this way. "
                                L"Restart as a normal user.");
            } else {
                SetSimpleStatus(dialog, ui::Status::Idle, L"Ready.", L"");
            }
            return TRUE;
        }

        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlID == IDC_STATUSICON) {
                FillRect(item->hDC, &item->rcItem,
                         GetSysColorBrush(COLOR_BTNFACE));
                ui::DrawStatus(item->hDC, item->rcItem, g_status);
                return TRUE;
            }
            return FALSE;
        }

        case WM_APP_LOG: {
            auto* line = reinterpret_cast<wchar_t*>(lParam);
            std::wstring text(line);
            free(line);

            while (!text.empty() && (text.front() == L' ')) text.erase(0, 1);
            if (!text.empty() && text.front() != L'-') {
                g_lastDetail = text;
                SetText(dialog, IDC_STATUSDETAIL, text);
            }
            return TRUE;
        }

        case WM_APP_DONE: {
            auto* verdict = reinterpret_cast<wchar_t*>(lParam);
            const std::wstring text(verdict);
            free(verdict);

            g_busy = false;
            EnableWindow(GetDlgItem(dialog, IDC_GO), TRUE);
            EnableWindow(GetDlgItem(dialog, IDC_TESTING), TRUE);

            if (wParam != 0) {
                SetSimpleStatus(
                    dialog, ui::Status::Success, L"Hooks loaded.",
                    SelectedMainMode(dialog) == HookMode::Sdr
                        ? L"The game renders SDR for the whole session."
                        : L"SDR only while a briefing video is on screen.");
            } else {
                SetSimpleStatus(dialog, ui::Status::Failure,
                                L"The hooks did not load.", text);
            }
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_GO: {
                    if (IsProcessElevated()) {
                        SetSimpleStatus(dialog, ui::Status::Failure,
                                        L"Cannot launch while elevated.",
                                        L"Restart this tool as a normal user.");
                        return TRUE;
                    }
                    EnableWindow(GetDlgItem(dialog, IDC_GO), FALSE);
                    EnableWindow(GetDlgItem(dialog, IDC_TESTING), FALSE);
                    SetSimpleStatus(dialog, ui::Status::Busy, L"Launching...", L"");

                    RunOptions options = DefaultRunOptions();
                    options.hooks.mode = SelectedMainMode(dialog);
                    options.dllPath =
                        StrategyDllPath(SelectedStrategy(dialog, IDC_MAINSTRATEGY));
                    if (!StartJob(dialog, options)) {
                        EnableWindow(GetDlgItem(dialog, IDC_GO), TRUE);
                        EnableWindow(GetDlgItem(dialog, IDC_TESTING), TRUE);
                        SetSimpleStatus(dialog, ui::Status::Failure,
                                        L"Could not start.", L"");
                    }
                    return TRUE;
                }

                case IDC_TESTING:
                    if (g_busy) {
                        MessageBoxW(dialog,
                                    L"A run is in progress. Wait for it to finish "
                                    L"before switching views.",
                                    L"Halo Wars 2 HDR Patcher", MB_ICONWARNING | MB_OK);
                        return TRUE;
                    }
                    LayoutView(dialog, !g_testingVisible);
                    return TRUE;

                case IDCANCEL:
                    if (g_busy) {
                        if (MessageBoxW(dialog,
                                        L"A launch is still in progress. Close "
                                        L"anyway?",
                                        L"Halo Wars 2 HDR Patcher",
                                        MB_ICONWARNING | MB_YESNO) != IDYES) {
                            return TRUE;
                        }
                    }
                    EndDialog(dialog, 0);
                    return TRUE;

                default:
                    break;
            }
            return FALSE;

        case WM_CLOSE:
            PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    g_instance = instance;

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    ui::StartGraphics();

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, SimpleProc, 0);

    ui::StopGraphics();
    return 0;
}
