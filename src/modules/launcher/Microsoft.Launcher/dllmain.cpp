#include "pch.h"
#include <interface/powertoy_module_interface.h>
#include <common/settings_objects.h>
#include <common/common.h>
#include <common/shared_constants.h>
#include "trace.h"
#include "Generated Files/resource.h"
#include <common/os-detect.h>
#include <launcher\Microsoft.Launcher\LauncherConstants.h>
#include <common/logger/logger.h>
#include <common\settings_helpers.h>
#include <filesystem>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    const wchar_t POWER_LAUNCHER_PID_SHARED_FILE[] = L"Local\\PowerLauncherPidSharedFile-3cbfbad4-199b-4e2c-9825-942d5d3d3c74";
    const wchar_t JSON_KEY_PROPERTIES[] = L"properties";
    const wchar_t JSON_KEY_WIN[] = L"win";
    const wchar_t JSON_KEY_ALT[] = L"alt";
    const wchar_t JSON_KEY_CTRL[] = L"ctrl";
    const wchar_t JSON_KEY_SHIFT[] = L"shift";
    const wchar_t JSON_KEY_CODE[] = L"code";
    const wchar_t JSON_KEY_OPEN_POWERLAUNCHER[] = L"open_powerlauncher";
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        Trace::RegisterProvider();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        Trace::UnregisterProvider();
        break;
    }
    return TRUE;
}

// These are the properties shown in the Settings page.
struct ModuleSettings
{
} g_settings;

// Implement the PowerToy Module Interface and all the required methods.
class Microsoft_Launcher : public PowertoyModuleIface
{
private:
    // The PowerToy state.
    bool m_enabled = false;

    // Load initial settings from the persisted values.
    void init_settings();

    // Handle to launch and terminate the launcher
    HANDLE m_hProcess;

    //contains the name of the powerToys
    std::wstring app_name;

    //contains the non localized key of the powertoy
    std::wstring app_key;

    // Time to wait for process to close after sending WM_CLOSE signal
    static const int MAX_WAIT_MILLISEC = 10000;

    // Hotkey to invoke the module
    Hotkey m_hotkey = { .key = 0 };

    // Helper function to extract the hotkey from the settings
    void parse_hotkey(PowerToysSettings::PowerToyValues& settings);

    // Handle to event used to invoke the Runner
    HANDLE m_hEvent;

    std::shared_ptr<Logger> logger;

public:
    // Constructor
    Microsoft_Launcher()
    {
        app_name = GET_RESOURCE_STRING(IDS_LAUNCHER_NAME);
        app_key = LauncherConstants::ModuleKey;
        std::filesystem::path logFilePath(PTSettingsHelper::get_module_save_folder_location(this->app_key));
        logFilePath.append(LogSettings::launcherLogPath);
        logger = std::make_shared<Logger>(LogSettings::launcherLoggerName, logFilePath.wstring(), PTSettingsHelper::get_log_settings_file_location());
        logger->info("Launcher object is constructing");
        init_settings();

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = false;
        sa.lpSecurityDescriptor = NULL;
        m_hEvent = CreateEventW(&sa, FALSE, FALSE, CommonSharedConstants::POWER_LAUNCHER_SHARED_EVENT);
    };

    ~Microsoft_Launcher()
    {
        logger->info("Launcher object is destroying");
        logger.reset();
        if (m_enabled)
        {
            terminateProcess();
        }
        m_enabled = false;
    }

    // Destroy the powertoy and free memory
    virtual void destroy() override
    {
        delete this;
    }

    // Return the localized display name of the powertoy
    virtual const wchar_t* get_name() override
    {
        return app_name.c_str();
    }

    // Return the non localized key of the powertoy, this will be cached by the runner
    virtual const wchar_t* get_key() override
    {
        return app_key.c_str();
    }

    // Return JSON with the configuration options.
    virtual bool get_config(wchar_t* buffer, int* buffer_size) override
    {
        HINSTANCE hinstance = reinterpret_cast<HINSTANCE>(&__ImageBase);

        // Create a Settings object.
        PowerToysSettings::Settings settings(hinstance, get_name());
        settings.set_description(GET_RESOURCE_STRING(IDS_LAUNCHER_SETTINGS_DESC));
        settings.set_overview_link(L"https://aka.ms/PowerToysOverview_PowerToysRun");

        return settings.serialize_to_buffer(buffer, buffer_size);
    }

    // Signal from the Settings editor to call a custom action.
    // This can be used to spawn more complex editors.
    virtual void call_custom_action(const wchar_t* action) override
    {
        static UINT custom_action_num_calls = 0;
        try
        {
            // Parse the action values, including name.
            PowerToysSettings::CustomActionObject action_object =
                PowerToysSettings::CustomActionObject::from_json_string(action);
        }
        catch (std::exception ex)
        {
            // Improper JSON.
        }
    }

    // Called by the runner to pass the updated settings values as a serialized JSON.
    virtual void set_config(const wchar_t* config) override
    {
        try
        {
            // Parse the input JSON string.
            PowerToysSettings::PowerToyValues values =
                PowerToysSettings::PowerToyValues::from_json_string(config, get_key());

            parse_hotkey(values);
            // If you don't need to do any custom processing of the settings, proceed
            // to persists the values calling:
            values.save_to_settings_file();
            // Otherwise call a custom function to process the settings before saving them to disk:
            // save_settings();
        }
        catch (std::exception ex)
        {
            // Improper JSON.
        }
    }

    // Enable the powertoy
    virtual void enable()
    {
        this->logger->info("Launcher is enabling");
        ResetEvent(m_hEvent);
        // Start PowerLauncher.exe only if the OS is 19H1 or higher
        if (UseNewSettings())
        {
            unsigned long powertoys_pid = GetCurrentProcessId();

            if (!is_process_elevated(false))
            {
                std::wstring executable_args;
                executable_args += L" -powerToysPid ";
                executable_args += std::to_wstring(powertoys_pid);
                executable_args += L" --centralized-kb-hook";

                SHELLEXECUTEINFOW sei{ sizeof(sei) };
                sei.fMask = { SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI };
                sei.lpFile = L"modules\\launcher\\PowerLauncher.exe";
                sei.nShow = SW_SHOWNORMAL;
                sei.lpParameters = executable_args.data();
                ShellExecuteExW(&sei);

                m_hProcess = sei.hProcess;
            }
            else
            {
                std::wstring action_runner_path = get_module_folderpath();

                std::wstring params;
                params += L"-run-non-elevated ";
                params += L"-target modules\\launcher\\PowerLauncher.exe ";
                params += L"-pidFile ";
                params += POWER_LAUNCHER_PID_SHARED_FILE;
                params += L" -powerToysPid " + std::to_wstring(powertoys_pid) + L" ";
                params += L"--centralized-kb-hook ";

                action_runner_path += L"\\action_runner.exe";
                // Set up the shared file from which to retrieve the PID of PowerLauncher
                HANDLE hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(DWORD), POWER_LAUNCHER_PID_SHARED_FILE);
                if (hMapFile)
                {
                    PDWORD pidBuffer = reinterpret_cast<PDWORD>(MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DWORD)));
                    if (pidBuffer)
                    {
                        *pidBuffer = 0;
                        m_hProcess = NULL;

                        if (run_non_elevated(action_runner_path, params, pidBuffer))
                        {
                            const int maxRetries = 80;
                            for (int retry = 0; retry < maxRetries; ++retry)
                            {
                                Sleep(50);
                                DWORD pid = *pidBuffer;
                                if (pid)
                                {
                                    m_hProcess = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
                                    break;
                                }
                            }
                        }
                    }
                    CloseHandle(hMapFile);
                }
            }
        }

        m_enabled = true;
    }

    // Disable the powertoy
    virtual void disable()
    {
        this->logger->info("Launcher is disabling");
        if (m_enabled)
        {
            ResetEvent(m_hEvent);
            terminateProcess();
        }

        m_enabled = false;
    }

    // Returns if the powertoys is enabled
    virtual bool is_enabled() override
    {
        return m_enabled;
    }

    // Return the invocation hotkey
    virtual size_t get_hotkeys(Hotkey* hotkeys, size_t buffer_size) override
    {
        if (m_hotkey.key)
        {
            if (hotkeys && buffer_size >= 1)
            {
                hotkeys[0] = m_hotkey;
            }

            return 1;
        }
        else
        {
            return 0;
        }
    }

    // Process the hotkey event
    virtual bool on_hotkey(size_t hotkeyId) override
    {
        // For now, hotkeyId will always be zero
        if (m_enabled)
        {
            if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0)
            {
                // The process exited, restart it
                enable();
            }

            SetEvent(m_hEvent);
            return true;
        }

        return false;
    }

    // Callback to send WM_CLOSE signal to each top level window.
    static BOOL CALLBACK requestMainWindowClose(HWND nextWindow, LPARAM closePid)
    {
        DWORD windowPid;
        GetWindowThreadProcessId(nextWindow, &windowPid);

        if (windowPid == (DWORD)closePid)
            ::PostMessage(nextWindow, WM_CLOSE, 0, 0);

        return true;
    }

    // Terminate process by sending WM_CLOSE signal and if it fails, force terminate.
    void terminateProcess()
    {
        DWORD processID = GetProcessId(m_hProcess);
        if (TerminateProcess(m_hProcess, 1) == 0)
        {
            auto err = get_last_error_message(GetLastError());
            this->logger->error(L"Launcher process was not terminated. {}", err.has_value() ? err.value() : L"");
        }

        // Temporarily disable sending a message to close
        /*
        EnumWindows(&requestMainWindowClose, processID);
        const DWORD result = WaitForSingleObject(m_hProcess, MAX_WAIT_MILLISEC);
        if (result == WAIT_TIMEOUT || result == WAIT_FAILED)
        {
            TerminateProcess(m_hProcess, 1);
        }
        */
    }
};

// Load the settings file.
void Microsoft_Launcher::init_settings()
{
    try
    {
        // Load and parse the settings file for this PowerToy.
        PowerToysSettings::PowerToyValues settings =
            PowerToysSettings::PowerToyValues::load_from_settings_file(get_key());

        parse_hotkey(settings);
    }
    catch (std::exception ex)
    {
        // Error while loading from the settings file. Let default values stay as they are.
    }
}

void Microsoft_Launcher::parse_hotkey(PowerToysSettings::PowerToyValues& settings)
{
    try
    {
        auto jsonHotkeyObject = settings.get_raw_json().GetNamedObject(JSON_KEY_PROPERTIES).GetNamedObject(JSON_KEY_OPEN_POWERLAUNCHER);
        m_hotkey.win = jsonHotkeyObject.GetNamedBoolean(JSON_KEY_WIN);
        m_hotkey.alt = jsonHotkeyObject.GetNamedBoolean(JSON_KEY_ALT);
        m_hotkey.shift = jsonHotkeyObject.GetNamedBoolean(JSON_KEY_SHIFT);
        m_hotkey.ctrl = jsonHotkeyObject.GetNamedBoolean(JSON_KEY_CTRL);
        m_hotkey.key = static_cast<unsigned char>(jsonHotkeyObject.GetNamedNumber(JSON_KEY_CODE));
    }
    catch (...)
    {
        m_hotkey.key = 0;
    }
}

extern "C" __declspec(dllexport) PowertoyModuleIface* __cdecl powertoy_create()
{
    return new Microsoft_Launcher();
}
