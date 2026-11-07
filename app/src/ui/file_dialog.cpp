#include "file_dialog.hpp"

#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace dsptb::ui {

namespace {

bool resultPath(IFileDialog* dialog, std::filesystem::path& out) {
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item)))
        return false;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw)
        return false;
    out = raw;
    CoTaskMemFree(raw);
    return true;
}

void setStartFolder(IFileDialog* dialog, const std::filesystem::path& folder) {
    if (folder.empty())
        return;
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item))))
        dialog->SetFolder(item.Get());
}

bool openDialog(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters, UINT nFilters,
                std::filesystem::path& out, DWORD extraOptions = 0) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return false;
    if (filters) {
        dialog->SetFileTypes(nFilters, filters);
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetTitle(title);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | extraOptions);
    if (FAILED(dialog->Show(owner)))
        return false; // cancelled
    return resultPath(dialog.Get(), out);
}

bool saveDialog(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters, UINT nFilters,
                const wchar_t* defaultExt, const std::filesystem::path& startFolder,
                const std::wstring& suggestedName, std::filesystem::path& out) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return false;
    dialog->SetFileTypes(nFilters, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(defaultExt);
    dialog->SetTitle(title);
    dialog->SetFileName(suggestedName.c_str());
    setStartFolder(dialog.Get(), startFolder);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    if (FAILED(dialog->Show(owner)))
        return false;
    return resultPath(dialog.Get(), out);
}

} // namespace

bool pickAudioFile(HWND owner, std::filesystem::path& out) {
    const COMDLG_FILTERSPEC filters[] = {{L"Audio files (*.wav, *.flac, *.mp3)", L"*.wav;*.flac;*.mp3"},
                                         {L"All files", L"*.*"}};
    return openDialog(owner, L"Open audio file", filters, 2, out, FOS_FILEMUSTEXIST);
}

bool pickSessionToOpen(HWND owner, std::filesystem::path& out) {
    const COMDLG_FILTERSPEC filters[] = {{L"Sessions (*.dsptb.json)", L"*.dsptb.json;*.json"},
                                         {L"All files", L"*.*"}};
    return openDialog(owner, L"Open session", filters, 2, out, FOS_FILEMUSTEXIST);
}

bool pickModuleZip(HWND owner, std::filesystem::path& out) {
    const COMDLG_FILTERSPEC filters[] = {{L"Module packages (*.zip)", L"*.zip"}};
    return openDialog(owner, L"Install module", filters, 1, out, FOS_FILEMUSTEXIST);
}

bool pickSessionToSave(HWND owner, const std::filesystem::path& startFolder,
                       const std::wstring& suggestedName, std::filesystem::path& out) {
    const COMDLG_FILTERSPEC filters[] = {{L"Sessions (*.dsptb.json)", L"*.dsptb.json"}};
    return saveDialog(owner, L"Save session", filters, 1, L"json", startFolder, suggestedName, out);
}

bool pickWavToSave(HWND owner, const std::filesystem::path& startFolder, const std::wstring& suggestedName,
                   std::filesystem::path& out) {
    const COMDLG_FILTERSPEC filters[] = {{L"WAV files (*.wav)", L"*.wav"}};
    return saveDialog(owner, L"Export processed WAV", filters, 1, L"wav", startFolder, suggestedName, out);
}

bool pickFolder(HWND owner, std::filesystem::path& out) {
    return openDialog(owner, L"Choose the recording folder", nullptr, 0, out, FOS_PICKFOLDERS);
}

} // namespace dsptb::ui
