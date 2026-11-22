#include <GL/gl.h>
#include <fcntl.h>
#include <algorithm>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "engine.hpp"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "platform_x11.hpp"

namespace fs = std::filesystem;

// ---------------------------------------------------------------- helpers

static bool inPath(const char* prog) {
  const char* p = std::getenv("PATH");
  if (!p) return false;
  std::string s = p;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(':', i);
    if (j == std::string::npos) j = s.size();
    if (access((s.substr(i, j - i) + "/" + prog).c_str(), X_OK) == 0) return true;
    i = j + 1;
  }
  return false;
}

// Run a program, return the first line of stdout ("" on failure).
static std::string capture(const std::vector<std::string>& args) {
  int fd[2];
  if (pipe(fd) != 0) return "";
  pid_t pid = fork();
  if (pid == 0) {
    dup2(fd[1], 1);
    int nul = open("/dev/null", 1);
    if (nul >= 0) dup2(nul, 2);
    close(fd[0]); close(fd[1]);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fd[1]);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
  close(fd[0]);
  waitpid(pid, nullptr, 0);
  return out;
}

static void spawnDetached(const std::vector<std::string>& args) {
  pid_t pid = fork();
  if (pid == 0) {
    if (fork() != 0) _exit(0);
    setsid();
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back((char*)a.c_str());
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  if (pid > 0) waitpid(pid, nullptr, 0);
}

static std::string firstLine(std::string s) {
  size_t n = s.find('\n');
  if (n != std::string::npos) s.resize(n);
  return s;
}

// Clipboard: shell out to whatever the desktop provides; fall back to an in-app buffer.
static std::string g_clip;
static const char* getClipboard(ImGuiContext*) {
  std::string s;
  if (inPath("wl-paste")) s = capture({"wl-paste", "-n"});
  if (s.empty() && inPath("xclip")) s = capture({"xclip", "-selection", "clipboard", "-o"});
  if (s.empty() && inPath("xsel")) s = capture({"xsel", "-ob"});
  if (!s.empty()) g_clip = s;
  return g_clip.c_str();
}
static void setClipboard(ImGuiContext*, const char* text) {
  g_clip = text;
  FILE* f = nullptr;
  if (inPath("wl-copy")) f = popen("wl-copy", "w");
  else if (inPath("xclip")) f = popen("xclip -selection clipboard", "w");
  if (f) { fwrite(text, 1, g_clip.size(), f); pclose(f); }
}

// ---------------------------------------------------------------- shared state

struct Shared {
  enum State { kIdle, kRunning, kDone, kStopped, kError };
  std::mutex m;
  std::vector<std::string> lines;
  cf::Stats stats;
  std::string label = "Nothing running", count;
  float frac = 0;
  State status = kIdle;
  std::string error;
  std::string creatorDir;
};

static std::string fmtBytes(long long b) {
  if (b <= 0) return "0 B";
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  int i = 0;
  double v = (double)b;
  while (v >= 1024 && i < 4) { v /= 1024; i++; }
  char buf[32];
  std::snprintf(buf, sizeof buf, i ? "%.2f %s" : "%.0f %s", v, u[i]);
  return buf;
}

static void applyTheme() {
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 0; s.ChildRounding = 10; s.FrameRounding = 7; s.GrabRounding = 7;
  s.FrameBorderSize = 1; s.ChildBorderSize = 1; s.FramePadding = ImVec2(10, 8); s.ItemSpacing = ImVec2(10, 10);
  s.ScrollbarRounding = 9;
  auto C = [&](ImGuiCol c, float r, float g, float b, float a = 1) { s.Colors[c] = ImVec4(r / 255, g / 255, b / 255, a); };
  C(ImGuiCol_WindowBg, 15, 17, 21);
  C(ImGuiCol_ChildBg, 23, 26, 33);
  C(ImGuiCol_Border, 38, 43, 54);
  C(ImGuiCol_Text, 231, 233, 238);
  C(ImGuiCol_TextDisabled, 139, 146, 163);
  C(ImGuiCol_FrameBg, 15, 17, 21);
  C(ImGuiCol_FrameBgHovered, 28, 32, 42);
  C(ImGuiCol_FrameBgActive, 28, 32, 42);
  C(ImGuiCol_Button, 30, 34, 44);
  C(ImGuiCol_ButtonHovered, 42, 48, 62);
  C(ImGuiCol_ButtonActive, 54, 62, 80);
  C(ImGuiCol_CheckMark, 108, 140, 255);
  C(ImGuiCol_PlotHistogram, 108, 140, 255);
  C(ImGuiCol_ScrollbarBg, 0, 0, 0, 0);
  C(ImGuiCol_ScrollbarGrab, 54, 62, 80);
  C(ImGuiCol_TextSelectedBg, 108, 140, 255, 0.35f);
}

static bool accentButton(const char* label, ImVec2 size, bool enabled, ImVec4 col, ImVec4 text) {
  ImGui::BeginDisabled(!enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, col);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 1.1f, col.y * 1.1f, col.z * 1.1f, 1));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 0.9f, col.y * 0.9f, col.z * 0.9f, 1));
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  bool r = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();
  return r;
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
  X11Window win;
  if (!win.init("CoomerFans Downloader", 1040, 740)) return 1;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  float scale = 1.0f;
  if (const char* e = std::getenv("GUI_SCALE")) scale = (float)std::atof(e) > 0 ? (float)std::atof(e) : 1.0f;

  const char* fonts[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                         "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf"};
  bool fontLoaded = false;
  for (auto f : fonts)
    if (access(f, R_OK) == 0) { io.Fonts->AddFontFromFileTTF(f, 15.0f * scale); fontLoaded = true; break; }
  if (!fontLoaded) { ImFontConfig cfg; cfg.SizePixels = 15.0f * scale; io.Fonts->AddFontDefault(&cfg); }

  applyTheme();
  ImGui::GetStyle().ScaleAllSizes(scale);
  ImGui::GetPlatformIO().Platform_GetClipboardTextFn = getClipboard;
  ImGui::GetPlatformIO().Platform_SetClipboardTextFn = setClipboard;
  ImGui_ImplOpenGL3_Init("#version 130");

  Shared sh;
  std::unique_ptr<cf::Downloader> job;
  std::thread worker;
  std::future<std::string> picker;

  static char url[1024] = "";
  static char outdir[1024] = "";
  int delayMs = 500;
  int mode = 0;  // 0 download, 1 list
  std::string formError;
  {
    const char* home = std::getenv("HOME");
    std::string d = std::string(home ? home : ".") + "/Downloads/coomerfans";
    std::snprintf(outdir, sizeof outdir, "%s", d.c_str());
  }
  if (argc > 1) std::snprintf(url, sizeof url, "%s", argv[1]);  // optional: prefill the URL
  const bool canPick = inPath("zenity") || inPath("kdialog");

  auto addLog = [&](const std::string& msg) {
    std::lock_guard<std::mutex> lk(sh.m);
    size_t i = 0;
    while (i <= msg.size()) {
      size_t j = msg.find('\n', i);
      if (j == std::string::npos) j = msg.size();
      sh.lines.push_back(msg.substr(i, j - i));
      i = j + 1;
    }
    if (sh.lines.size() > 5000) sh.lines.erase(sh.lines.begin(), sh.lines.begin() + 1000);
  };

  auto startJob = [&] {
    formError.clear();
    std::string u = url, o = outdir;
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) { formError = "Enter a valid creator URL (https://...)."; return; }
    if (o.empty()) { formError = "Choose an output folder."; return; }
    if (!inPath("curl")) { formError = "`curl` was not found on PATH. Please install curl."; return; }
    if (worker.joinable()) worker.join();
    {
      std::lock_guard<std::mutex> lk(sh.m);
      sh.lines.clear(); sh.stats = {}; sh.label = "Starting..."; sh.count.clear(); sh.frac = 0;
      sh.status = Shared::kRunning; sh.error.clear(); sh.creatorDir = o;
    }
    cf::Options opts;
    opts.outputDir = o;
    opts.delayMs = std::max(0, delayMs);
    cf::Callbacks cb;
    cb.log = addLog;
    cb.stats = [&](const cf::Stats& s) { std::lock_guard<std::mutex> lk(sh.m); sh.stats = s; };
    cb.current = [&](const std::string& l, const std::string& c, float f) {
      std::lock_guard<std::mutex> lk(sh.m); sh.label = l; sh.count = c; sh.frac = f;
    };
    job = std::make_unique<cf::Downloader>(opts, cb);
    cf::Downloader* j = job.get();
    bool listMode = mode == 1;
    worker = std::thread([&, j, u, listMode] {
      Shared::State end = Shared::kDone;
      try {
        if (listMode) j->downloadFileList(u); else j->downloadContent(u);
        if (j->stopped()) end = Shared::kStopped;
      } catch (const std::exception& e) {
        addLog(std::string("\n[Error] ") + e.what());
        std::lock_guard<std::mutex> lk(sh.m);
        sh.error = e.what();
        end = Shared::kError;
      }
      std::lock_guard<std::mutex> lk(sh.m);
      if (!j->creatorDir().empty()) sh.creatorDir = j->creatorDir();
      if (end == Shared::kStopped) sh.label = "Stopped - start again to resume";
      else if (end == Shared::kError) sh.label = "Failed";
      sh.status = end;
    });
  };

  while (true) {
    Shared::State status;
    { std::lock_guard<std::mutex> lk(sh.m); status = sh.status; }
    if (!win.pump(status == Shared::kRunning ? 16 : 100)) break;

    if (picker.valid() && picker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      std::string d = picker.get();
      if (!d.empty()) std::snprintf(outdir, sizeof outdir, "%s", d.c_str());
    }

    ImGui_ImplOpenGL3_NewFrame();
    win.beginFrame();
    ImGui::NewFrame();

    Shared snap;  // copy of what the UI needs (avoid holding the lock while drawing)
    std::vector<std::string>* linesPtr = nullptr;
    sh.m.lock();
    snap.stats = sh.stats; snap.label = sh.label; snap.count = sh.count; snap.frac = sh.frac;
    snap.status = sh.status; snap.creatorDir = sh.creatorDir;
    linesPtr = &sh.lines;

    const bool running = snap.status == Shared::kRunning;
    const float W = (float)win.width(), H = (float)win.height();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(W, H));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImVec4 accent(108 / 255.f, 140 / 255.f, 255 / 255.f, 1), muted(139 / 255.f, 146 / 255.f, 163 / 255.f, 1);
    ImVec4 ok(62 / 255.f, 207 / 255.f, 142 / 255.f, 1), bad(1, 107 / 255.f, 107 / 255.f, 1);

    // Header
    {
      ImVec2 p = ImGui::GetCursorScreenPos();
      float r = 5 * scale;
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() / 2), r, ImGui::GetColorU32(accent));
      ImGui::Dummy(ImVec2(r * 2, 0));
      ImGui::SameLine();
    }
    ImGui::TextUnformatted("CoomerFans Downloader");
    {
      const char* st = "Idle";
      ImVec4 c = muted;
      switch (snap.status) {
        case Shared::kRunning: st = "Running"; c = accent; break;
        case Shared::kDone: st = "Done"; c = ok; break;
        case Shared::kStopped: st = "Stopped"; break;
        case Shared::kError: st = "Error"; c = bad; break;
        default: break;
      }
      ImGui::SameLine(W - ImGui::CalcTextSize(st).x - 28 * scale);
      ImGui::TextColored(c, "%s", st);
    }
    ImGui::Separator();

    const float leftW = std::min(390.f * scale, W * 0.42f);
    const float bodyH = ImGui::GetContentRegionAvail().y;

    // ---------------- form
    ImGui::BeginChild("form", ImVec2(leftW, bodyH), true, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::TextColored(muted, "Creator URL");
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##url", "https://coomerfans.com/u/onlyfans/313401/rbbca", url, sizeof url);

    struct ModeOpt { const char* title; const char* sub; };
    ModeOpt modes[2] = {{"Download content", "Open every post and save media. Resumable."},
                        {"File list only", "Collect post titles page by page. No media."}};
    for (int i = 0; i < 2; i++) {
      ImGui::PushID(i);
      bool sel = mode == i;
      ImVec2 p = ImGui::GetCursorScreenPos();
      float w = ImGui::GetContentRegionAvail().x, h = 52 * scale;
      if (ImGui::InvisibleButton("##mode", ImVec2(w, h))) mode = i;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImU32 border = sel ? ImGui::GetColorU32(accent) : ImGui::GetColorU32(ImGuiCol_Border);
      if (sel) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(108, 140, 255, 24), 8);
      else if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 8), 8);
      dl->AddRect(p, ImVec2(p.x + w, p.y + h), border, 8);
      dl->AddText(ImVec2(p.x + 12, p.y + 8 * scale), ImGui::GetColorU32(ImGuiCol_Text), modes[i].title);
      dl->AddText(ImVec2(p.x + 12, p.y + 8 * scale + ImGui::GetTextLineHeight() + 2), ImGui::GetColorU32(muted), modes[i].sub);
      ImGui::PopID();
    }

    ImGui::TextColored(muted, "Save to");
    float browseW = canPick ? 90 * scale : 0;
    ImGui::SetNextItemWidth(-(browseW + (canPick ? ImGui::GetStyle().ItemSpacing.x : 0)));
    ImGui::InputText("##out", outdir, sizeof outdir);
    if (canPick) {
      ImGui::SameLine();
      if (ImGui::Button("Browse...", ImVec2(browseW, 0)) && !picker.valid()) {
        picker = std::async(std::launch::async, [] {
          if (inPath("zenity")) return firstLine(capture({"zenity", "--file-selection", "--directory", "--title=Choose output folder"}));
          return firstLine(capture({"kdialog", "--getexistingdirectory", "."}));
        });
      }
    }
    ImGui::TextColored(muted, "Delay between requests (ms)");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##delay", &delayMs, 100, 500);
    if (delayMs < 0) delayMs = 0;
    ImGui::EndDisabled();

    // Actions pinned to the bottom
    float btnH = ImGui::GetFrameHeight();
    float errH = formError.empty() ? 0 : ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - btnH - errH - 4);
    if (!formError.empty()) ImGui::TextColored(bad, "%s", formError.c_str());
    float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    float openW = 110 * scale, stopW = 80 * scale;
    if (accentButton("Start", ImVec2(avail - openW - stopW - 2 * sp, 0), !running, accent, ImVec4(0.04f, 0.05f, 0.09f, 1))) {
      sh.m.unlock();
      startJob();
      sh.m.lock();
    }
    ImGui::SameLine();
    if (accentButton("Stop", ImVec2(stopW, 0), running, ImVec4(0.2f, 0.1f, 0.1f, 1), bad)) {
      if (job) job->stop();
      snap.label = "Stopping after the current step...";
      sh.label = snap.label;
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder", ImVec2(openW, 0))) {
      std::string d = snap.creatorDir.empty() ? std::string(outdir) : snap.creatorDir;
      std::error_code ec;
      fs::create_directories(d, ec);
      spawnDetached({"xdg-open", d});
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();

    // ---------------- right column
    ImGui::SameLine();
    ImGui::BeginGroup();
    float rightW = ImGui::GetContentRegionAvail().x;
    {
      const char* names[4] = {"Pages", "Posts", "Files", "Downloaded"};
      std::string vals[4] = {std::to_string(snap.stats.pages), std::to_string(snap.stats.posts),
                             std::to_string(snap.stats.files), fmtBytes(snap.stats.bytes)};
      float cw = (rightW - 3 * ImGui::GetStyle().ItemSpacing.x) / 4;
      for (int i = 0; i < 4; i++) {
        if (i) ImGui::SameLine();
        ImGui::BeginChild(names[i], ImVec2(cw, 70 * scale), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextUnformatted(vals[i].c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextColored(muted, "%s", names[i]);
        ImGui::EndChild();
      }
    }
    ImGui::BeginChild("current", ImVec2(rightW, 78 * scale), true);
    ImGui::TextUnformatted(snap.label.c_str());
    if (!snap.count.empty()) { ImGui::SameLine(rightW - ImGui::CalcTextSize(snap.count.c_str()).x - 24 * scale); ImGui::TextColored(muted, "%s", snap.count.c_str()); }
    ImGui::ProgressBar(snap.status == Shared::kDone ? 1.f : snap.frac, ImVec2(-1, 6 * scale), "");
    ImGui::EndChild();

    ImGui::BeginChild("log", ImVec2(rightW, ImGui::GetContentRegionAvail().y), true);
    ImGui::TextColored(muted, "Activity log");
    ImGui::SameLine(rightW - 60 * scale);
    if (ImGui::SmallButton("Clear")) linesPtr->clear();
    ImGui::Separator();
    ImGui::BeginChild("lines", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    const bool followTail = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;  // was at bottom last frame
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * scale));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.73f, 0.75f, 0.81f, 1));
    ImGuiListClipper clip;
    clip.Begin((int)linesPtr->size());
    while (clip.Step())
      for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted((*linesPtr)[i].c_str());
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (followTail) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::EndGroup();

    ImGui::End();
    sh.m.unlock();

    ImGui::Render();
    glViewport(0, 0, win.width(), win.height());
    glClearColor(15 / 255.f, 17 / 255.f, 21 / 255.f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    win.swap();
  }

  if (job) job->stop();
  if (worker.joinable()) worker.join();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui::DestroyContext();
  win.shutdown();
  return 0;
}
