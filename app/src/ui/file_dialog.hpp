#pragma once

#include <Windows.h>

#include <filesystem>

namespace dsptb::ui {

// The system file dialogs (IFileOpenDialog / IFileSaveDialog). Each blocks until the user chooses or cancels
// and returns true and fills `out` on OK. COM must be initialised on the calling thread.

// Open: WAV, FLAC and MP3.
bool pickAudioFile(HWND owner, std::filesystem::path& out);
// Open a session (*.dsptb.json).
bool pickSessionToOpen(HWND owner, std::filesystem::path& out);
// Save a session; `suggestedName` is the file name offered (for example "Untitled.dsptb.json").
bool pickSessionToSave(HWND owner, const std::filesystem::path& startFolder,
                       const std::wstring& suggestedName, std::filesystem::path& out);
// Save a WAV (the export target).
bool pickWavToSave(HWND owner, const std::filesystem::path& startFolder, const std::wstring& suggestedName,
                   std::filesystem::path& out);
// Open a module package (*.zip).
bool pickModuleZip(HWND owner, std::filesystem::path& out);
// Choose a folder (the recording folder).
bool pickFolder(HWND owner, std::filesystem::path& out);

} // namespace dsptb::ui
