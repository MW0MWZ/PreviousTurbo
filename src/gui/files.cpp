/*
  PreviousTurbo - files.cpp

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  The system's file and folder choosers. SDL may answer on another thread:
  answers are queued and handed over in the main loop.
*/

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>

#include "gui_internal.h"
#include "main.h"
extern "C" {
#include "paths.h"
}

namespace ptgui {

namespace {

struct Request {
    std::vector<FileFilter> filters;
    std::vector<SDL_DialogFileFilter> sdl_filters;
    std::string start;
    std::function<void(const std::string&)> done;
};

struct Answer {
    std::function<void(const std::string&)> done;
    std::string path;
};

std::mutex g_mutex;
std::deque<Answer> g_answers;

void SDLCALL on_chosen(void* userdata, const char* const* files, int) {
    std::unique_ptr<Request> req(static_cast<Request*>(userdata));
    std::string path;
    if (!files) SDL_Log("file chooser: %s", SDL_GetError());
    else if (files[0]) path = files[0];
    std::lock_guard<std::mutex> lock(g_mutex);
    g_answers.push_back(Answer{std::move(req->done), std::move(path)});
}

// Does path lie in the program's own data folder (the ROMs that come with it)?
bool in_data_dir(const std::string& path) {
    const char* data = Paths_GetDataDir();
    if (!data || !*data || path.empty()) return false;
    std::error_code ec;
    const auto a = std::filesystem::weakly_canonical(std::filesystem::path(data), ec);
    const auto b = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec) return false;
    const auto rel = b.lexically_relative(a);
    return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

void choose(Pick kind, const std::string& start, std::vector<FileFilter> filters,
            std::function<void(const std::string&)> done, Place place) {
    auto* req = new Request{std::move(filters), {}, start, std::move(done)};
    // With nothing chosen yet (or a ROM that came with the program), start in
    // the matching folder of Documents/PreviousTurbo, as SiliconGhost does.
    const char* folder = place == Place::Roms ? Paths_GetRomsDir()
                         : place == Place::Base ? Paths_GetUserData() : Paths_GetDiskImagesDir();
    if ((req->start.empty() || (place == Place::Roms && in_data_dir(req->start))) && folder)
        req->start = std::string(folder) + PATHSEP;
    for (const FileFilter& f : req->filters) req->sdl_filters.push_back(SDL_DialogFileFilter{f.name, f.pattern});
    // Start in the folder of what is chosen now (or the file itself, for saving).
    if (kind != Pick::Save && !req->start.empty() && req->start.back() != '/' && req->start.back() != '\\') {
        std::error_code ec;
        if (!std::filesystem::is_directory(req->start, ec)) req->start = folder_of(req->start);
    }
    const char* location = req->start.empty() ? nullptr : req->start.c_str();
    const SDL_DialogFileFilter* f = req->sdl_filters.empty() ? nullptr : req->sdl_filters.data();
    const int nf = static_cast<int>(req->sdl_filters.size());
    switch (kind) {
        case Pick::Open: SDL_ShowOpenFileDialog(on_chosen, req, st().window, f, nf, location, false); break;
        case Pick::Save: SDL_ShowSaveFileDialog(on_chosen, req, st().window, f, nf, location); break;
        case Pick::Folder: SDL_ShowOpenFolderDialog(on_chosen, req, st().window, location, false); break;
    }
}

std::string choose_now(Pick kind, const std::string& start, std::vector<FileFilter> filters, Place place) {
    bool answered = false;
    std::string result;
    choose(kind, start, std::move(filters), [&](const std::string& path) {
        result = path;
        answered = true;
    }, place);
    run_modal([&] { return !answered; });
    return result;
}

void deliver_choices() {
    std::deque<Answer> answers;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        answers.swap(g_answers);
    }
    for (Answer& a : answers) {
        if (a.done) a.done(a.path);
        wake(2);
    }
}

}  // namespace ptgui
