#include "App.h"

#include "BoardGeometry.h"
#include "Desktop.h"
#include "PieceModels.h"
#include "Themes.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <stdexcept>
#include <utility>

using namespace math;
using chess::Color;
using chess::PieceType;
using ui::FontId;
using ui::Rect;
using ui::Rgba;

namespace {

constexpr float PanelWidth = 344, PanelMargin = 16;

enum Widget {
    HelpButton = 1,
    UndoButton,
    RedoButton,
    HintButton,
    NewGameButton,
    FlipButton,
    ModeSelector,
    LevelSelector = ModeSelector + 2,
    ThemeSelector = LevelSelector + 2,
    SoundButton = ThemeSelector + 2,
    LanguageButton,
    AboutButton,
    ModalWidgets = 100, // the widgets of the cards over the game and of the promotion choice
    HelpClose,
    AboutClose,
    PromotionTile,
    AboutLinks = 110,
};

constexpr ui::Color TextColor = Rgba(236, 239, 245);
constexpr ui::Color MutedColor = Rgba(150, 158, 176);

chess::SearchLimits LevelLimits(int level, std::uint64_t seed)
{
    switch (level) {
    case 1: return {1, 3000, 180, seed};
    case 2: return {2, 3000, 70, seed};
    case 3: return {4, 2500, 22, seed};
    case 4: return {64, 1200, 0, seed};
    default: return {64, 3500, 0, seed};
    }
}

int MaterialValue(PieceType type)
{
    switch (type) {
    case PieceType::Pawn: return 1;
    case PieceType::Knight:
    case PieceType::Bishop: return 3;
    case PieceType::Rook: return 5;
    case PieceType::Queen: return 9;
    default: return 0;
    }
}

const Theme& ThemeAt(int index) { return Themes()[static_cast<std::size_t>(index)]; }

// The letter of a key for the shortcuts: by its label in Latin layouts (Z on a German keyboard is where it is
// marked), and by its place on a US keyboard in the others, such as the Russian one, as Windows does.
char ShortcutLetter(const SDL_Keysym& key)
{
    if (key.sym >= SDLK_a && key.sym <= SDLK_z)
        return static_cast<char>(key.sym);
    if (key.sym >= 0x80 && key.scancode >= SDL_SCANCODE_A && key.scancode <= SDL_SCANCODE_Z)
        return static_cast<char>('a' + (key.scancode - SDL_SCANCODE_A));
    return 0;
}

// The computer level of a digit key, on the main keys or the keypad, or 0.
int LevelKey(const SDL_Keysym& key)
{
    if (key.scancode >= SDL_SCANCODE_1 && key.scancode <= SDL_SCANCODE_5)
        return key.scancode - SDL_SCANCODE_1 + 1;
    if (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_5)
        return key.scancode - SDL_SCANCODE_KP_1 + 1;
    return 0;
}

} // namespace

// Setup --------------------------------------------------------------------------------------------

App::SdlSession::SdlSession()
{
    SDL_SetHint(SDL_HINT_APP_NAME, "3D Chess");
    // A game in a window rather than a full-screen one: the desktop goes on compositing it, the screen saver may
    // start, and leaving full screen for another window does not minimize it.
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::format("The display could not be opened: {}", SDL_GetError()));
}

App::SdlSession::~SdlSession() { SDL_Quit(); }

App::App()
{
    m_settings = Settings::Load();
    m_language = m_settings.language;
    m_theme = m_settings.theme;
    m_level = m_settings.level;
    m_mode = m_settings.mode;
    m_sound.SetEnabled(m_settings.sound);
    m_panelVisible = m_settings.panel;
    m_panelSlide = m_panelVisible ? 1.0f : 0.0f;

    const resources::Image icon = resources::Png("chess3d.png");
    CreateMainWindow(icon);
    m_context = std::make_unique<gl::Context>(m_window.get());
    render::Program::SetVersion(m_context->Version());
    m_painter = std::make_unique<ui::Painter>();
    m_painter->SetIcon(icon);
    SDL_ShowWindow(m_window.get());
    if (m_settings.maximized)
        SDL_MaximizeWindow(m_window.get());
    UpdateSize();
    BuildFonts();
    ShowLoading();

#ifdef NDEBUG
    constexpr float detail = 1.0f;
#else
    constexpr float detail = 0.6f; // unoptimized builds model the pieces more coarsely, to start quickly
#endif
    const auto shapes = pieces::BuildPieceShapes(detail);
    m_renderer = std::make_unique<render::Renderer>(shapes, m_context->Version());
    m_renderer->SetTheme(ThemeAt(m_theme));
    std::array<float, 6> heights{}, radii{};
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        heights[i] = shapes[i].height;
        radii[i] = shapes[i].radius;
    }
    m_view = std::make_unique<BoardView>(heights, radii);

    if (auto saved = chess::Game::Deserialize(m_settings.game))
        m_game = std::move(*saved);
    m_announcedEnd = m_game.Status().Over();
    m_view->Rebuild(m_game, true);
    m_camera.Reset(ViewSide());
    m_lastFrame = std::chrono::steady_clock::now();
}

App::~App()
{
    m_computer.Cancel();
    m_hinter.Cancel();
}

void App::CreateMainWindow(const resources::Image& icon)
{
    gl::Context::RequestPixelFormat();
    // Hidden until it has its size, which depends on the desktop's scaling, which is known from a window.
    m_window.reset(SDL_CreateWindow("3D Chess", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 800,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN));
    if (!m_window)
        throw std::runtime_error(std::format("The window could not be created: {}", SDL_GetError()));
    SDL_Window* window = m_window.get();
    desktop::SetIcon(window, icon);
    SDL_StopTextInput(); // keys only: no input method in the way
    m_textScale = desktop::TextScale(window);

    // The last size, or most of the screen's work area, centered in it. Room is left for the frame, so that on a small
    // screen with a large scale the window can be seen whole even at its least size.
    const int display = std::max(0, SDL_GetWindowDisplayIndex(window));
    SDL_Rect work{0, 0, 1920, 1080};
    if (SDL_GetDisplayUsableBounds(display, &work) != 0)
        SDL_GetDisplayBounds(display, &work);
    const auto units = [&](int size) { return static_cast<int>(std::lround(static_cast<float>(size) * m_textScale)); };
    const int roomWidth = work.w * 92 / 100, roomHeight = work.h * 92 / 100;
    int width = m_settings.windowWidth, height = m_settings.windowHeight;
    if (width <= 0 || height <= 0) {
        width = std::min(units(1500), roomWidth);
        height = std::min(units(920), roomHeight);
    }
    width = std::min(width, work.w);
    height = std::min(height, work.h);
    SDL_SetWindowMinimumSize(window, std::min(units(900), roomWidth), std::min(units(600), roomHeight));
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, work.x + (work.w - width) / 2, work.y + (work.h - height) / 2);
    m_normalSize = m_previousNormalSize = {width, height};

    m_arrowCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    m_handCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
}

void App::UpdateSize()
{
    SDL_GetWindowSize(m_window.get(), &m_windowWidth, &m_windowHeight);
    SDL_GL_GetDrawableSize(m_window.get(), &m_width, &m_height);
    const float pixelsPerUnit = m_windowWidth > 0 && m_width > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_scale = std::clamp(m_textScale * pixelsPerUnit, 0.5f, 4.0f);
}

void App::BuildFonts()
{
    const float scale = Scale();
    m_fonts[0] = std::make_unique<text::Font>(15 * scale, text::Weight::Regular);
    m_fonts[1] = std::make_unique<text::Font>(19 * scale, text::Weight::SemiBold);
    m_fonts[2] = std::make_unique<text::Font>(31 * scale, text::Weight::SemiBold);
    m_painter->SetFonts(m_fonts[0].get(), m_fonts[1].get(), m_fonts[2].get());
    m_fontScale = scale;
}

void App::ShowLoading()
{
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, m_width, m_height);
    gl::glClearColor(0.016f, 0.017f, 0.022f, 1);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);
    m_painter->Begin(m_width, m_height);
    const Rect screen{0, 0, static_cast<float>(m_width), static_cast<float>(m_height)};
    m_painter->TextCentered(FontId::Display, screen, T(Str::Title), TextColor);
    m_painter->Flush();
    m_context->SwapBuffers();
}

int App::Run()
{
    SDL_Event event;
    while (m_running) {
        // Nothing moving and nothing new, or nothing to be seen: wait for input instead of drawing the same picture.
        const bool hidden = (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        if (hidden || (!m_dirty && !NeedsAnimation())) {
            if (SDL_WaitEvent(&event))
                HandleEvent(event);
            m_lastFrame = std::chrono::steady_clock::now(); // the idle time is not animation time
        }
        while (SDL_PollEvent(&event))
            HandleEvent(event);
        if (!m_running || hidden || (!m_dirty && !NeedsAnimation()))
            continue;

        m_dirty = false;
        const auto start = std::chrono::steady_clock::now();
        Frame();
        // Swapping the buffers waits for the screen; where it does not, the loop sleeps a little.
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2))
            SDL_Delay(1);
    }
    Save();
    return 0;
}

void App::HandleEvent(const SDL_Event& event)
{
    m_dirty = true;
    switch (event.type) {
    case SDL_QUIT: m_running = false; break;
    case SDL_WINDOWEVENT: HandleWindowEvent(event.window); break;
    case SDL_MOUSEMOTION: PointerAt(event.motion.x, event.motion.y); break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        PointerAt(event.button.x, event.button.y);
        const bool down = event.type == SDL_MOUSEBUTTONDOWN;
        if (event.button.button == SDL_BUTTON_LEFT) {
            m_pointer.leftDown = down;
            (down ? m_pointer.leftPressed : m_pointer.leftReleased) = true;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT) {
            m_pointer.rightDown = down;
            (down ? m_pointer.rightPressed : m_pointer.rightReleased) = true;
        }
        break;
    }
    case SDL_MOUSEWHEEL:
        // Away from the user is positive, in notches; touchpads scroll in fractions of one.
        m_pointer.wheel += event.wheel.preciseY;
        break;
    case SDL_KEYDOWN: HandleKey(event.key); break;
    default: break;
    }
}

void App::HandleWindowEvent(const SDL_WindowEvent& event)
{
    switch (event.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        UpdateSize();
        if (!m_fullScreen && (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0) {
            m_previousNormalSize = m_normalSize;
            m_normalSize = {m_windowWidth, m_windowHeight};
            m_normalSizeTime = event.timestamp;
        }
        break;
    case SDL_WINDOWEVENT_MAXIMIZED:
        // The new size may come a moment before the word that the window is maximized: it was not a size of its own.
        if (event.timestamp - m_normalSizeTime < 250)
            m_normalSize = m_previousNormalSize;
        break;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        m_textScale = desktop::TextScale(m_window.get());
        UpdateSize();
        break;
    case SDL_WINDOWEVENT_LEAVE:
        // So that nothing stays highlighted under a pointer that has gone.
        if (!m_pointer.leftDown && !m_pointer.rightDown)
            m_pointer.x = m_pointer.y = -1;
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        // A button let go of in another window is not heard here.
        if (m_pointer.leftDown)
            m_pointer.leftReleased = true;
        if (m_pointer.rightDown)
            m_pointer.rightReleased = true;
        m_pointer.leftDown = m_pointer.rightDown = false;
        break;
    default: break;
    }
}

void App::PointerAt(int windowX, int windowY)
{
    const float pixelsPerUnit = m_windowWidth > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_pointer.x = static_cast<float>(windowX) * pixelsPerUnit;
    m_pointer.y = static_cast<float>(windowY) * pixelsPerUnit;
}

// The frame ----------------------------------------------------------------------------------------

void App::Frame()
{
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::min(std::chrono::duration<float>(now - m_lastFrame).count(), 0.1f);
    m_lastFrame = now;
    m_time += seconds;
    if (m_width <= 0 || m_height <= 0)
        return;
    if (m_scale != m_fontScale)
        BuildFonts();
    m_renderer->Resize(m_width, m_height);

    Update(seconds);

    const Theme& theme = ThemeAt(m_theme);
    render::Scene scene;
    scene.view = m_camera.View();
    scene.projection = m_camera.Projection(static_cast<float>(m_width) / static_cast<float>(m_height));
    scene.eye = m_camera.Eye();
    scene.time = m_time;
    scene.pieces = m_view->Draws(m_selected, m_hoverPiece, theme.accent, m_time);
    scene.contacts = m_view->Contacts();

    auto bit = [](int square) { return square >= 0 ? std::uint64_t{1} << square : 0; };
    scene.marks.selected = bit(m_selected);
    for (const chess::Move& move : m_selectedMoves)
        (m_game.Current().IsCapture(move) ? scene.marks.capture : scene.marks.quiet) |= bit(move.to);
    if (!m_game.Moves().empty()) {
        const chess::Move last = m_game.Moves().back().move;
        scene.marks.lastMove = bit(last.from) | bit(last.to);
    }
    if (m_game.Status().check)
        scene.marks.check = bit(m_game.Current().KingSquare(m_game.Current().SideToMove()));
    scene.marks.hover = bit(m_hoverSquare);
    if (m_hint && m_hintGlow > 0.01f)
        scene.arrow = render::Arrow{board::SquareCenter(m_hint->from), board::SquareCenter(m_hint->to), theme.accent * 3.5f, 0.9f * m_hintGlow};
    scene.panels = FrostedPanels();
    m_renderer->Render(scene);

    m_cursorHand = false;
    m_painter->Begin(m_width, m_height);
    DrawInterface();
    m_painter->Flush();
    HandleBoardPointer();

    // The widgets are drawn as they were before they heard the click: what it changed shows in the next frame.
    if (m_pointer.leftPressed || m_pointer.leftReleased || m_pointer.rightPressed || m_pointer.rightReleased)
        m_dirty = true;
    m_dialogOpening = false;
    m_pointer.leftPressed = m_pointer.leftReleased = m_pointer.rightPressed = m_pointer.rightReleased = false;
    m_pointer.wheel = 0;
    m_pointer.consumed = false;
    if (m_cursorHand != m_cursorShownHand) {
        SDL_SetCursor(m_cursorHand ? m_handCursor.get() : m_arrowCursor.get());
        m_cursorShownHand = m_cursorHand;
    }
    m_context->SwapBuffers();
}

void App::Update(float seconds)
{
    const float target = m_panelVisible ? 1.0f : 0.0f;
    m_panelSlide += (target - m_panelSlide) * (1 - std::exp(-seconds * 12));
    if (std::abs(target - m_panelSlide) < 0.001f)
        m_panelSlide = target;
    const float panelSpace = (PanelWidth + PanelMargin) * Scale() * m_panelSlide;
    m_camera.SetSpace(1 - panelSpace / static_cast<float>(std::max(m_width, 1)));
    m_camera.Update(seconds);
    m_view->Update(seconds);

    const bool over = m_game.Status().Over();
    m_bannerFade = std::clamp(m_bannerFade + (over ? seconds : -seconds) * 3, 0.0f, 1.0f);
    m_hintGlow = std::clamp(m_hintGlow + (m_hint ? seconds : -seconds) * 4, 0.0f, 1.0f);
    for (Toast& toast : m_toasts)
        toast.age += seconds;
    std::erase_if(m_toasts, [](const Toast& toast) { return toast.age > 2.8f; });

    UpdateHint();
    UpdateComputer(seconds);

    if (over && !m_announcedEnd) {
        m_announcedEnd = true;
        m_sound.Play(SoundBank::Effect::GameOver);
    }
    if (!over)
        m_announcedEnd = false;
}

bool App::NeedsAnimation() const
{
    return m_camera.Moving() || m_view->Animating() || m_computerThinking || m_computerResult || m_hintThinking || m_selected >= 0 ||
           m_game.Status().check || !m_toasts.empty() || (m_panelSlide > 0 && m_panelSlide < 1) || (m_bannerFade > 0 && m_bannerFade < 1) ||
           m_hintGlow > 0 || m_promotion || m_gesture != Gesture::None || m_pointer.leftDown || m_pointer.rightDown ||
           (!m_game.Status().Over() && !IsHuman(m_game.Current().SideToMove()));
}

// The game -----------------------------------------------------------------------------------------

bool App::IsHuman(Color side) const
{
    switch (m_mode) {
    case PlayWhite: return side == Color::White;
    case PlayBlack: return side == Color::Black;
    case TwoPlayers: return true;
    default: return false;
    }
}

bool App::HumanToMove() const
{
    return !m_game.Status().Over() && IsHuman(m_game.Current().SideToMove());
}

void App::StopComputer()
{
    m_computer.Cancel();
    m_computerThinking = false;
    m_computerResult.reset();
    m_hinter.Cancel();
    m_hintThinking = false;
}

void App::UpdateComputer(float seconds)
{
    if (m_promotion || m_game.Status().Over() || IsHuman(m_game.Current().SideToMove()))
        return;
    m_computerClock += seconds;
    if (!m_computerThinking && !m_computerResult) {
        static std::mt19937_64 random{std::random_device{}()};
        m_computer.Start(m_game.Current(), m_game.RepetitionHistory(), LevelLimits(m_level, random()));
        m_computerThinking = true;
        m_computerClock = 0;
    }
    if (m_computerThinking) {
        // Asked before the result: a search that has ended has left its result by then.
        const bool busy = m_computer.Busy();
        if (auto result = m_computer.TakeResult()) {
            m_computerResult = result;
            m_computerThinking = false;
        }
        else if (!busy) {
            m_computerThinking = false; // cancelled; a new search starts next frame
        }
    }
    const float pause = m_mode == Demo ? 0.9f : 0.45f;
    if (m_computerResult && !m_view->Animating() && m_computerClock >= pause && m_gesture == Gesture::None) {
        const chess::Move move = m_computerResult->best;
        m_computerResult.reset();
        if (!move.IsNull())
            PlayMove(move);
    }
}

void App::UpdateHint()
{
    if (!m_hintThinking)
        return;
    const bool busy = m_hinter.Busy();
    if (auto result = m_hinter.TakeResult()) {
        m_hintThinking = false;
        if (!result->best.IsNull() && HumanToMove()) {
            m_hint = result->best;
            ShowToast(std::string(T(Str::HintToast)) + m_game.Current().ToSan(result->best));
        }
    }
    else if (!busy) {
        m_hintThinking = false;
    }
}

void App::NewGame()
{
    StopComputer();
    m_computer.NewGame();
    m_game.Reset(chess::Position::Start());
    m_view->Rebuild(m_game, true);
    m_camera.Reset(ViewSide());
    m_announcedEnd = false;
    AfterPositionChange();
}

void App::PlayMove(chess::Move move)
{
    if (!m_game.Play(move))
        return;
    m_view->PlayMove(m_game.Moves().back(), true);
    Announce(m_game.Moves().back());
    AfterPositionChange();
}

void App::Announce(const chess::MoveRecord& record)
{
    if (m_game.Status().Over())
        return; // the end has its own sound
    if (m_game.Status().check)
        m_sound.Play(SoundBank::Effect::Check);
    else if (record.move.IsCastling())
        m_sound.Play(SoundBank::Effect::Castle);
    else if (!record.captured.Empty())
        m_sound.Play(SoundBank::Effect::Capture);
    else
        m_sound.Play(SoundBank::Effect::Move);
}

void App::Undo()
{
    StopComputer();
    if (m_promotion) {
        m_promotion.reset();
        return;
    }
    if (!m_game.CanUndo()) {
        ShowToast(std::string(T(Str::NothingToUndo)));
        return;
    }
    auto undoOne = [&] {
        const chess::MoveRecord record = m_game.Moves().back();
        m_game.Undo();
        m_view->PlayMove(record, false);
    };
    undoOne();
    // Against the computer, back to the player's own turn.
    if ((m_mode == PlayWhite || m_mode == PlayBlack) && !IsHuman(m_game.Current().SideToMove()) && m_game.CanUndo())
        undoOne();
    m_sound.Play(SoundBank::Effect::Move);
    AfterPositionChange();
}

void App::Redo()
{
    StopComputer();
    if (!m_game.CanRedo())
        return;
    m_game.Redo();
    m_view->PlayMove(m_game.Moves().back(), true);
    if ((m_mode == PlayWhite || m_mode == PlayBlack) && !IsHuman(m_game.Current().SideToMove()) && m_game.CanRedo()) {
        m_game.Redo();
        m_view->PlayMove(m_game.Moves().back(), true);
    }
    Announce(m_game.Moves().back());
    AfterPositionChange();
}

void App::SetMode(int mode)
{
    StopComputer();
    m_mode = (mode % 4 + 4) % 4;
    if (m_mode == PlayWhite || m_mode == PlayBlack)
        m_camera.FaceSide(ViewSide());
    ClearSelection();
    Save();
}

void App::SetLevel(int level)
{
    level = std::clamp(level, 1, 5);
    if (level == m_level)
        return;
    m_level = level;
    if (m_computerThinking)
        StopComputer(); // starts again at the new level
    Save();
}

void App::SetTheme(int theme)
{
    m_theme = (theme % ThemeCount + ThemeCount) % ThemeCount;
    m_renderer->SetTheme(ThemeAt(m_theme));
    Save();
}

void App::ToggleSound()
{
    m_sound.SetEnabled(!m_sound.Enabled());
    Save();
}

void App::ToggleLanguage()
{
    m_language = m_language == Language::English ? Language::Russian : Language::English;
    Save();
}

void App::RequestHint()
{
    if (!HumanToMove() || m_hintThinking || m_promotion)
        return;
    m_hint.reset();
    m_hinter.Start(m_game.Current(), m_game.RepetitionHistory(), {64, 1200, 0, 0});
    m_hintThinking = true;
}

void App::AfterPositionChange()
{
    ClearSelection();
    m_hint.reset();
    m_promotion.reset();
    m_followMoves = true;
    Save();
}

void App::ClearSelection()
{
    m_selected = -1;
    m_selectedMoves.clear();
    m_hoverSquare = -1;
}

void App::Select(int square)
{
    m_selected = square;
    m_selectedMoves = m_game.Current().LegalMovesFrom(square);
}

void App::TryMove(int from, int to)
{
    std::vector<chess::Move> choices;
    for (const chess::Move& move : m_game.Current().LegalMovesFrom(from))
        if (move.to == to)
            choices.push_back(move);
    if (choices.empty())
        return;
    if (choices.size() > 1) {
        m_promotion = choices; // the player picks the piece
        m_view->Release(from);
        return;
    }
    PlayMove(choices.front());
}

void App::CopyGame(bool fen)
{
    const std::string text = fen ? m_game.Current().ToFen() : m_game.ToPgn();
    if (SDL_SetClipboardText(text.c_str()) != 0)
        return;
    ShowToast(std::string(T(fen ? Str::FenCopied : Str::PgnCopied)));
}

void App::PasteGame()
{
    std::string text;
    if (char* clipboard = SDL_GetClipboardText()) {
        text = clipboard;
        SDL_free(clipboard);
    }
    auto game = chess::Game::Parse(text);
    if (!game) {
        ShowToast(std::string(T(Str::NothingToPaste)));
        return;
    }
    StopComputer();
    m_game = std::move(*game);
    m_view->Rebuild(m_game, true);
    m_announcedEnd = m_game.Status().Over();
    AfterPositionChange();
    ShowToast(std::string(T(Str::GameLoaded)));
}

void App::OpenLink(const char* url)
{
    if (SDL_OpenURL(url) == 0)
        return;
    // Where no browser could be started, the link goes to the clipboard instead.
    SDL_SetClipboardText(url);
    ShowToast(std::string(T(Str::LinkCopied)));
}

void App::Save()
{
    m_settings.theme = m_theme;
    m_settings.level = m_level;
    m_settings.mode = m_mode;
    m_settings.sound = m_sound.Enabled();
    m_settings.panel = m_panelVisible;
    m_settings.language = m_language;
    m_settings.game = m_game.Serialize();
    if (m_window) {
        const std::uint32_t flags = SDL_GetWindowFlags(m_window.get());
        if (!m_fullScreen && (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
            SDL_GetWindowSize(m_window.get(), &m_normalSize.width, &m_normalSize.height);
        m_settings.windowWidth = m_normalSize.width;
        m_settings.windowHeight = m_normalSize.height;
        m_settings.maximized = m_fullScreen ? m_maximizedBeforeFullScreen : (flags & SDL_WINDOW_MAXIMIZED) != 0;
    }
    m_settings.Save();
}

void App::ToggleFullScreen()
{
    const bool fullScreen = !m_fullScreen;
    if (fullScreen)
        m_maximizedBeforeFullScreen = (SDL_GetWindowFlags(m_window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
    // Borderless over the whole screen, at its own resolution.
    if (SDL_SetWindowFullscreen(m_window.get(), fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
        m_fullScreen = fullScreen;
}

// Input --------------------------------------------------------------------------------------------

Vec3 App::PointerOnPlane(float height) const
{
    Vec3 origin, direction;
    m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
    if (std::abs(direction.y) < 1e-5f)
        return origin;
    const float t = (height - origin.y) / direction.y;
    return origin + direction * std::max(t, 0.0f);
}

int App::SquareUnderPointer() const { return board::SquareAt(PointerOnPlane(0)); }

void App::HandleBoardPointer()
{
    const bool blocked = m_pointer.consumed || m_dialog != Dialog::None || m_promotion.has_value();
    const float scale = Scale();
    auto isTarget = [&](int square) {
        return square >= 0 && std::ranges::any_of(m_selectedMoves, [square](const chess::Move& move) { return move.to == square; });
    };
    auto pointedSquare = [&] {
        Vec3 origin, direction;
        m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
        const int piece = m_view->Pick(origin, direction);
        return piece >= 0 ? piece : SquareUnderPointer();
    };
    auto movable = [&](int square) {
        if (square < 0 || !HumanToMove())
            return false;
        const chess::Piece piece = m_game.Current().At(square);
        return !piece.Empty() && piece.color == m_game.Current().SideToMove();
    };

    // What the pointer is over.
    const bool insideWindow = m_pointer.x >= 0 && m_pointer.y >= 0 && m_pointer.x < static_cast<float>(m_width) && m_pointer.y < static_cast<float>(m_height);
    if (m_gesture == Gesture::None || m_gesture == Gesture::DragPiece) {
        const int square = insideWindow && !blocked ? pointedSquare() : -1;
        m_hoverPiece = m_gesture == Gesture::None && movable(square) ? square : -1;
        m_hoverSquare = isTarget(m_gesture == Gesture::DragPiece ? SquareUnderPointer() : square) ? (m_gesture == Gesture::DragPiece ? SquareUnderPointer() : square) : -1;
    }
    else {
        m_hoverPiece = m_hoverSquare = -1;
    }
    if (m_hoverPiece >= 0 || m_hoverSquare >= 0 || m_gesture == Gesture::DragPiece)
        m_cursorHand = true;

    if (!blocked && m_pointer.leftPressed) {
        const int square = pointedSquare();
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
        if (m_selected >= 0 && isTarget(square)) {
            TryMove(m_selected, square);
        }
        else if (movable(square)) {
            if (square != m_selected)
                Select(square);
            m_gesture = Gesture::PressPiece;
            m_pressSquare = square;
        }
        else {
            m_gesture = Gesture::TurnCamera;
            m_pressSquare = -1;
        }
    }
    if (!m_pointer.consumed && m_pointer.rightPressed && m_gesture == Gesture::None) {
        m_gesture = Gesture::TurnCamera;
        m_pressSquare = -1;
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
    }

    const float travel = std::abs(m_pointer.x - m_pressX) + std::abs(m_pointer.y - m_pressY);
    if (m_gesture == Gesture::PressPiece && travel > 6 * scale && m_pointer.leftDown)
        m_gesture = Gesture::DragPiece;
    if (m_gesture == Gesture::DragPiece)
        m_view->Hold(m_pressSquare, PointerOnPlane(0.4f) + Vec3{0, 0.05f, 0});
    if (m_gesture == Gesture::TurnCamera) {
        m_camera.Rotate(-(m_pointer.x - m_dragX) * 0.0055f / scale, (m_pointer.y - m_dragY) * 0.0045f / scale);
        m_dragX = m_pointer.x;
        m_dragY = m_pointer.y;
    }

    if (m_pointer.leftReleased || m_pointer.rightReleased) {
        if (m_gesture == Gesture::DragPiece && m_pointer.leftReleased) {
            const int square = SquareUnderPointer();
            if (isTarget(square))
                TryMove(m_pressSquare, square);
            else
                m_view->Release(m_pressSquare);
        }
        else if (m_gesture == Gesture::TurnCamera && travel < 4 * scale && m_pointer.leftReleased) {
            ClearSelection(); // a click beside the pieces
        }
        if ((m_pointer.leftReleased && m_gesture != Gesture::None && !(m_gesture == Gesture::TurnCamera && m_pointer.rightDown)) ||
            (m_pointer.rightReleased && m_gesture == Gesture::TurnCamera && !m_pointer.leftDown))
            m_gesture = Gesture::None;
    }

    if (!blocked && m_pointer.wheel != 0)
        m_camera.Zoom(m_pointer.wheel);
}

void App::HandleKey(const SDL_KeyboardEvent& event)
{
    const SDL_Keysym& key = event.keysym;
    const bool control = (key.mod & KMOD_CTRL) != 0;
    const bool shift = (key.mod & KMOD_SHIFT) != 0;
    const bool alt = (key.mod & KMOD_ALT) != 0;
    const bool repeat = event.repeat != 0;
    const char letter = ShortcutLetter(key);

    if (alt) {
        if ((key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER) && !repeat)
            ToggleFullScreen();
        return; // the rest of Alt is the desktop's
    }
    // While the piece of a promotion is chosen, only its keys count.
    if (m_promotion) {
        if (repeat)
            return;
        if (key.sym == SDLK_ESCAPE) {
            m_promotion.reset();
            return;
        }
        PieceType type = PieceType::None;
        switch (letter) {
        case 'q': type = PieceType::Queen; break;
        case 'r': type = PieceType::Rook; break;
        case 'b': type = PieceType::Bishop; break;
        case 'n': type = PieceType::Knight; break;
        default: return;
        }
        for (const chess::Move& move : *m_promotion) {
            if (move.promotion == type) {
                PlayMove(move);
                return;
            }
        }
        return;
    }
    if (m_dialog != Dialog::None && key.sym == SDLK_ESCAPE) {
        if (!repeat)
            m_dialog = Dialog::None;
        return;
    }

    // Taking back and replaying go on while the key is held; the other keys act once.
    if (control) {
        switch (letter) {
        case 'z': shift ? Redo() : Undo(); return;
        case 'y': Redo(); return;
        default: break;
        }
        if (repeat)
            return;
        switch (letter) {
        case 'c': CopyGame(shift); return;
        case 'v': PasteGame(); return;
        case 'n': NewGame(); return;
        default: return;
        }
    }
    switch (key.sym) {
    case SDLK_BACKSPACE:
    case SDLK_LEFT: Undo(); return;
    case SDLK_RIGHT: Redo(); return;
    default: break;
    }
    if (repeat)
        return;

    switch (key.sym) {
    case SDLK_HOME: m_camera.Reset(ViewSide()); return;
    case SDLK_TAB:
        m_panelVisible = !m_panelVisible;
        Save();
        return;
    case SDLK_F1: ToggleDialog(Dialog::Help); return;
    case SDLK_F11: ToggleFullScreen(); return;
    case SDLK_ESCAPE:
        if (m_fullScreen)
            ToggleFullScreen();
        else
            ClearSelection();
        return;
    default: break;
    }
    switch (letter) {
    case 'a': ToggleDialog(Dialog::About); return;
    case 'n': NewGame(); return;
    case 'h': RequestHint(); return;
    case 'f': m_camera.Flip(); return;
    case 'v': m_camera.ToggleTopView(); return;
    case 'm': SetMode(m_mode + 1); return;
    case 't': SetTheme(m_theme + (shift ? -1 : 1)); return;
    case 's': ToggleSound(); return;
    case 'l': ToggleLanguage(); return;
    default: break;
    }
    if (const int level = LevelKey(key))
        SetLevel(level);
}

// The interface ------------------------------------------------------------------------------------

Rect App::PanelRect() const
{
    const float scale = Scale();
    const float width = PanelWidth * scale, margin = PanelMargin * scale;
    return {static_cast<float>(m_width) - (width + margin) * m_panelSlide, margin, width, static_cast<float>(m_height) - 2 * margin};
}

Rect App::FreeArea() const
{
    const Rect panel = PanelRect();
    return {0, 0, std::min(panel.x, static_cast<float>(m_width)), static_cast<float>(m_height)};
}

namespace {

constexpr float HelpWidth = 660, HelpRow = 27, HelpPad = 28;
constexpr float AboutWidth = 520, AboutPad = 28, AboutIcon = 76, LinkRow = 36, LinkGap = 6;
constexpr float PromotionTileSize = 116, PromotionGap = 14, PromotionPad = 22;

// The about card is in English, whatever the language of the interface.
constexpr std::string_view AboutDescription =
    "A 3D chess game written in modern C++ on OpenGL 4.6, with a computer opponent of five levels, from Beginner to Master. "
    "Everything you see and hear is made in code: there is no model, texture or sound file.";

struct Link {
    std::string_view name;
    std::string_view address; // as shown
    const char* url;
};

constexpr std::array<Link, 5> AuthorLinks{{
    {"GitHub", "github.com/makarov-mm", "https://github.com/makarov-mm"},
    {"GitHub Sponsors", "github.com/sponsors/makarov-mm", "https://github.com/sponsors/makarov-mm"},
    {"LinkedIn", "linkedin.com/in/makarov-mm", "https://www.linkedin.com/in/makarov-mm/"},
    {"Threads", "threads.net/@m.m.makarov", "https://www.threads.net/@m.m.makarov"},
    {"Instagram", "instagram.com/m.m.makarov", "https://www.instagram.com/m.m.makarov/"},
}};

} // namespace

std::vector<std::string> App::WrapText(FontId font, std::string_view words, float width) const
{
    std::vector<std::string> lines;
    std::string line;
    while (!words.empty()) {
        const std::size_t space = std::min(words.find(' '), words.size());
        const std::string_view word = words.substr(0, space);
        words.remove_prefix(std::min(space + 1, words.size()));
        std::string longer = line.empty() ? std::string(word) : std::format("{} {}", line, word);
        if (!line.empty() && m_painter->Measure(font, longer) > width) {
            lines.push_back(std::move(line));
            line = word;
        }
        else {
            line = std::move(longer);
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

std::array<App::HelpLine, 21> App::HelpLines() const
{
    return {{
        {T(Str::KeyClick), Str::HelpClick},
        {T(Str::KeyDrag), Str::HelpDrag},
        {T(Str::KeyRightDrag), Str::HelpRotate},
        {T(Str::KeyWheel), Str::HelpZoom},
        {"Ctrl+Z  ←  Backspace", Str::HelpUndo},
        {"Ctrl+Y  →", Str::HelpRedo},
        {"H", Str::HelpHint},
        {"N", Str::HelpNew},
        {"F", Str::HelpFlip},
        {"V", Str::HelpTopView},
        {"1 – 5", Str::HelpLevel},
        {"M", Str::HelpMode},
        {"T", Str::HelpTheme},
        {"S", Str::HelpSound},
        {"L", Str::HelpLanguage},
        {"Tab", Str::HelpPanel},
        {"F11  Alt+Enter", Str::HelpFullScreen},
        {"Ctrl+C", Str::HelpCopy},
        {"Ctrl+V", Str::HelpPaste},
        {"A", Str::HelpAbout},
        {"Esc  F1", Str::HelpClose},
    }};
}

float App::HelpKeysWidth() const
{
    float width = 200 * Scale();
    for (const HelpLine& line : HelpLines())
        width = std::max(width, m_painter->Measure(FontId::Body, line.keys) + 16 * Scale());
    return width;
}

// The rows of the help close up, down to the height of a line, where the window is too low for them.
float App::HelpRowHeight() const
{
    const float scale = Scale();
    const auto rows = static_cast<float>(HelpLines().size());
    const float room = static_cast<float>(m_height) - (20 + 2 * HelpPad + 52) * scale;
    return std::max(m_painter->LineHeight(FontId::Body), std::min(room / rows, HelpRow * scale));
}

Rect App::CenteredCard(float width, float height) const
{
    const Rect free = FreeArea();
    const float x = width <= free.width ? free.x + (free.width - width) / 2 : std::max(0.0f, (static_cast<float>(m_width) - width) / 2);
    return {x, std::max(10 * Scale(), (static_cast<float>(m_height) - height) / 2), width, height};
}

Rect App::HelpCard() const
{
    const float scale = Scale();
    // Wider where the desktop's font is wider than the card was laid out for.
    float actionsWidth = 0;
    const auto lines = HelpLines();
    for (const HelpLine& line : lines)
        actionsWidth = std::max(actionsWidth, m_painter->Measure(FontId::Body, T(line.action)));
    const float width = std::max(HelpWidth * scale, 2 * HelpPad * scale + HelpKeysWidth() + actionsWidth);
    return CenteredCard(width, (2 * HelpPad + 52) * scale + static_cast<float>(lines.size()) * HelpRowHeight());
}

Rect App::AboutCard() const
{
    const float scale = Scale();
    const ui::Painter& p = *m_painter;
    const auto lines = static_cast<float>(WrapText(FontId::Body, AboutDescription, (AboutWidth - 2 * AboutPad) * scale).size());
    const auto links = static_cast<float>(AuthorLinks.size());
    const float height = (2 * AboutPad + AboutIcon + 20 + 16 + 2 + 20 + links * (LinkRow + LinkGap) - LinkGap) * scale +
                         lines * (p.LineHeight(FontId::Body) + 3 * scale) + p.LineHeight(FontId::Heading) + p.LineHeight(FontId::Body);
    return CenteredCard(AboutWidth * scale, height);
}

Rect App::PromotionCard() const
{
    const float scale = Scale();
    const float width = (4 * PromotionTileSize + 3 * PromotionGap + 2 * PromotionPad) * scale;
    const float height = (PromotionTileSize + 2 * PromotionPad + 44) * scale;
    return CenteredCard(width, height);
}

Rect App::BannerCard() const
{
    const float scale = Scale();
    const chess::GameStatus status = m_game.Status();
    const std::string_view title = status.reason == chess::EndReason::Checkmate ? T(Str::Checkmate)
                                   : status.reason == chess::EndReason::Stalemate ? T(Str::Stalemate)
                                                                                  : T(Str::Draw);
    const float width = std::max(m_painter->Measure(FontId::Display, title) + 80 * scale, 300 * scale);
    const Rect free = FreeArea();
    return {free.x + (free.width - width) / 2, 26 * scale - (1 - m_bannerFade) * 20 * scale, width, 92 * scale};
}

std::vector<render::FrostedPanel> App::FrostedPanels() const
{
    std::vector<render::FrostedPanel> panels;
    const float scale = Scale();
    auto add = [&](Rect rect, float radius, float opacity) { panels.push_back({rect.x, rect.y, rect.width, rect.height, radius * scale, opacity}); };
    if (m_panelSlide > 0.001f)
        add(PanelRect(), 18, 1);
    if (m_bannerFade > 0.001f)
        add(BannerCard(), 18, m_bannerFade);
    if (m_promotion)
        add(PromotionCard(), 20, 1);
    if (m_dialog == Dialog::Help)
        add(HelpCard(), 20, 1);
    else if (m_dialog == Dialog::About)
        add(AboutCard(), 20, 1);
    return panels;
}

void App::ShowToast(std::string text)
{
    if (m_toasts.size() >= 3)
        m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({std::move(text), 0});
}

std::string App::PlayerName(Color side) const
{
    if (m_mode == TwoPlayers)
        return std::string(T(Str::Player));
    if (IsHuman(side))
        return std::string(T(Str::You));
    return std::format("{} · {}", T(Str::Computer), T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)));
}

void App::ToggleDialog(Dialog dialog)
{
    m_dialog = m_dialog == dialog ? Dialog::None : dialog;
    m_dialogOpening = m_dialog != Dialog::None;
}

bool App::PressedOutside(Rect card) const
{
    // A quick click, such as a tap on a touchpad, presses and lets go within one frame: when it opens a card, its
    // press must not then close it.
    return m_pointer.leftPressed && !m_dialogOpening && !card.Contains(m_pointer.x, m_pointer.y);
}

App::Interaction App::Interact(int id, Rect rect, bool enabled)
{
    // While a card or the promotion choice is open, only its own widgets answer, from the frame after the one that
    // opened it.
    const bool modal = m_dialog != Dialog::None || m_promotion.has_value();
    const bool interactive = enabled && (!modal || (id > ModalWidgets && !m_dialogOpening));
    Interaction result;
    result.hover = interactive && rect.Contains(m_pointer.x, m_pointer.y);
    if (result.hover)
        m_cursorHand = true;
    if (result.hover && m_pointer.leftPressed) {
        m_activeWidget = id;
        m_pointer.consumed = true;
    }
    if (m_pointer.leftReleased && m_activeWidget == id) {
        result.clicked = result.hover;
        m_activeWidget = 0;
        m_pointer.consumed = true;
    }
    result.pressed = result.hover && m_activeWidget == id && m_pointer.leftDown;
    if (result.clicked)
        m_sound.Play(SoundBank::Effect::Click);
    return result;
}

bool App::Button(int id, Rect rect, std::string_view label, bool enabled, bool accent)
{
    const Interaction state = Interact(id, rect, enabled);
    ui::Color fill = !enabled        ? Rgba(255, 255, 255, 7)
                     : state.pressed ? Rgba(255, 255, 255, 52)
                     : state.hover   ? Rgba(255, 255, 255, 34)
                                     : Rgba(255, 255, 255, 17);
    if (accent && enabled)
        fill = ui::Mix(fill, ui::FromLinear(ThemeAt(m_theme).accent, static_cast<float>(fill.a) / 255.0f * 2.2f), 0.6f);
    m_painter->Fill(rect, 9 * Scale(), fill);
    m_painter->TextCentered(FontId::Body, rect, label, enabled ? TextColor : ui::Fade(TextColor, 0.35f));
    return state.clicked;
}

int App::Selector(int id, Rect rect, std::string_view label, std::string_view value, float labelWidth)
{
    const float scale = Scale();
    m_painter->Text(FontId::Body, rect.x, rect.y + (rect.height - m_painter->LineHeight(FontId::Body)) / 2, label, MutedColor);
    const float arrow = rect.height;
    const Rect right{rect.Right() - arrow, rect.y, arrow, rect.height};
    // The value from 36% of the row, or further where a label is longer: fonts differ in width from one desktop to another.
    const Rect left{std::max(rect.x + rect.width * 0.36f - arrow - 4 * scale, rect.x + labelWidth + 8 * scale), rect.y, arrow, rect.height};
    const Rect valueRect{left.Right(), rect.y, right.x - left.Right(), rect.height};
    m_painter->Fill({valueRect.x + 2 * scale, rect.y, valueRect.width - 4 * scale, rect.height}, 9 * scale, Rgba(255, 255, 255, 9));
    m_painter->TextCentered(FontId::Body, valueRect, m_painter->Elided(FontId::Body, value, valueRect.width - 8 * scale), TextColor);
    int change = 0;
    if (Button(id, left, ""))
        change = -1;
    if (Button(id + 1, right, ""))
        change = 1;
    const float size = 5 * scale;
    m_painter->Arrow(left.x + left.width / 2, left.y + left.height / 2, size * 0.8f, size, -1, TextColor);
    m_painter->Arrow(right.x + right.width / 2, right.y + right.height / 2, size * 0.8f, size, 1, TextColor);
    return change;
}

void App::DrawInterface()
{
    DrawPanel();
    DrawBanner();
    DrawToasts();
    if (m_promotion)
        DrawPromotion();
    if (m_dialog == Dialog::Help)
        DrawHelp();
    else if (m_dialog == Dialog::About)
        DrawAbout();
}

void App::DrawStatus(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const chess::GameStatus status = m_game.Status();
    const Color side = m_game.Current().SideToMove();
    const ui::Color accent = ui::FromLinear(ThemeAt(m_theme).accent);
    p.Fill(area, 12 * scale, Rgba(255, 255, 255, 14));

    // A disc of the side to move.
    const float discX = area.x + 22 * scale, discY = area.y + area.height / 2;
    p.Disc(discX, discY, 10 * scale, Rgba(120, 125, 140));
    p.Disc(discX, discY, 9 * scale, side == Color::White ? Rgba(238, 232, 220) : Rgba(22, 22, 26));

    std::string title, detail;
    ui::Color titleColor = TextColor;
    if (status.Over()) {
        title = status.reason == chess::EndReason::Checkmate ? T(Str::Checkmate) : status.reason == chess::EndReason::Stalemate ? T(Str::Stalemate) : T(Str::Draw);
        if (status.result == chess::GameResult::WhiteWins)
            detail = T(Str::WhiteWins);
        else if (status.result == chess::GameResult::BlackWins)
            detail = T(Str::BlackWins);
        else if (status.reason == chess::EndReason::Repetition)
            detail = T(Str::ByRepetition);
        else if (status.reason == chess::EndReason::FiftyMoves)
            detail = T(Str::ByFiftyMoves);
        else if (status.reason == chess::EndReason::InsufficientMaterial)
            detail = T(Str::ByMaterial);
        titleColor = accent;
    }
    else {
        title = status.check ? T(Str::Check) : T(side == Color::White ? Str::WhiteToMove : Str::BlackToMove);
        if (status.check)
            titleColor = Rgba(255, 120, 90);
        detail = m_computerThinking || m_computerResult ? std::string(T(Str::Thinking))
                 : status.check                         ? std::string(T(side == Color::White ? Str::WhiteToMove : Str::BlackToMove))
                                                        : PlayerName(side);
    }
    const bool busy = m_computerThinking || m_computerResult || m_hintThinking;
    const float textX = area.x + 44 * scale, textWidth = area.Right() - (busy ? 40 : 10) * scale - textX;
    const float lineHeight = p.LineHeight(FontId::Heading);
    p.Text(FontId::Heading, textX, area.y + area.height / 2 - lineHeight + 3 * scale, title, titleColor);
    p.Text(FontId::Body, textX, area.y + area.height / 2 + 2 * scale, p.Elided(FontId::Body, detail, textWidth), MutedColor);
    if (busy)
        p.Spinner(area.Right() - 22 * scale, area.y + area.height / 2, 8 * scale, 2.2f * scale, m_time * 5.0f, accent);
}

void App::DrawMoveList(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill(area, 12 * scale, Rgba(0, 0, 0, 40));
    const auto moves = m_game.Moves();
    if (moves.empty()) {
        p.TextCentered(FontId::Body, area, T(Str::NoMovesYet), MutedColor);
        return;
    }

    const float rowHeight = std::round(25 * scale);
    const Rect inner = area.Inset(8 * scale);
    const int visible = std::max(1, static_cast<int>(inner.height / rowHeight));
    const bool blackFirst = m_game.Start().SideToMove() == Color::Black;
    const int firstNumber = m_game.Start().FullmoveNumber();
    const int rows = (static_cast<int>(moves.size()) + (blackFirst ? 1 : 0) + 1) / 2;
    const float maxScroll = static_cast<float>(std::max(0, rows - visible));

    if (area.Contains(m_pointer.x, m_pointer.y) && m_pointer.wheel != 0 && m_dialog == Dialog::None) {
        m_moveScroll = std::clamp(m_moveScroll - m_pointer.wheel * 3, 0.0f, maxScroll);
        m_followMoves = m_moveScroll >= maxScroll;
        m_pointer.wheel = 0;
    }
    if (m_followMoves)
        m_moveScroll = maxScroll;
    m_moveScroll = std::clamp(m_moveScroll, 0.0f, maxScroll);

    const int first = static_cast<int>(m_moveScroll);
    const float numberWidth = 44 * scale, columnWidth = (inner.width - numberWidth) / 2;
    const ui::Color accent = ui::FromLinear(ThemeAt(m_theme).accent);
    for (int row = first; row < std::min(rows, first + visible); ++row) {
        const float y = inner.y + static_cast<float>(row - first) * rowHeight;
        const std::string number = std::format("{}.", firstNumber + row);
        p.Text(FontId::Body, inner.x + numberWidth - 10 * scale - p.Measure(FontId::Body, number), y + 3 * scale, number, MutedColor);
        for (int column = 0; column < 2; ++column) {
            const int ply = row * 2 + column - (blackFirst ? 1 : 0);
            if (ply < 0 || ply >= static_cast<int>(moves.size()))
                continue;
            const float x = inner.x + numberWidth + static_cast<float>(column) * columnWidth;
            if (ply == static_cast<int>(moves.size()) - 1)
                p.Fill({x - 6 * scale, y + 1 * scale, columnWidth - 6 * scale, rowHeight - 2 * scale}, 7 * scale, ui::Fade(accent, 0.28f));
            p.Text(FontId::Body, x, y + 3 * scale, moves[ply].san, TextColor);
        }
    }
    if (maxScroll > 0) {
        // A thin scroll bar.
        const float barHeight = inner.height * static_cast<float>(visible) / static_cast<float>(rows);
        const float barY = inner.y + (inner.height - barHeight) * (m_moveScroll / maxScroll);
        p.Fill({area.Right() - 5 * scale, barY, 3 * scale, barHeight}, 1.5f * scale, Rgba(255, 255, 255, 50));
    }
}

void App::DrawPanel()
{
    if (m_panelSlide <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect panel = PanelRect();
    if (panel.Contains(m_pointer.x, m_pointer.y) && (m_pointer.leftPressed || m_pointer.rightPressed))
        m_pointer.consumed = true;
    p.Shadow(panel, 22 * scale, Rgba(0, 0, 0, 70));
    p.Fill(panel, 18 * scale, Rgba(16, 18, 26, 120));
    p.Outline(panel, 18 * scale, Rgba(255, 255, 255, 24));

    const float pad = 20 * scale;
    const float x = panel.x + pad, width = panel.width - 2 * pad;
    float y = panel.y + pad;

    p.Text(FontId::Heading, x, y, T(Str::Title), TextColor);
    const float small = 30 * scale;
    if (Button(HelpButton, {panel.Right() - pad - small, y - 2 * scale, small, small}, "?"))
        ToggleDialog(Dialog::Help);
    if (Button(AboutButton, {panel.Right() - pad - 2 * small - 6 * scale, y - 2 * scale, small, small}, "i"))
        ToggleDialog(Dialog::About);
    y += 44 * scale;

    DrawStatus({x, y, width, 62 * scale});
    y += 62 * scale + 14 * scale;

    // The players, and who is ahead in material.
    int material = 0;
    for (int square = 0; square < 64; ++square) {
        const chess::Piece piece = m_game.Current().At(square);
        material += (piece.color == Color::White ? 1 : -1) * MaterialValue(piece.type);
    }
    for (const Color side : {Color::White, Color::Black}) {
        const float rowHeight = 28 * scale, textY = y + (rowHeight - p.LineHeight(FontId::Body)) / 2;
        p.Disc(x + 9 * scale, y + rowHeight / 2, 7 * scale, Rgba(120, 125, 140));
        p.Disc(x + 9 * scale, y + rowHeight / 2, 6 * scale, side == Color::White ? Rgba(238, 232, 220) : Rgba(22, 22, 26));
        const int lead = side == Color::White ? material : -material;
        const std::string leadText = lead > 0 ? std::format("+{}", lead) : std::string();
        const float leadWidth = p.Measure(FontId::Body, leadText), nameX = x + 26 * scale;
        const std::string name = std::format("{}  ·  {}", T(side == Color::White ? Str::White : Str::Black), PlayerName(side));
        p.Text(FontId::Body, nameX, textY, p.Elided(FontId::Body, name, x + width - leadWidth - 8 * scale - nameX), TextColor);
        if (lead > 0)
            p.Text(FontId::Body, x + width - leadWidth, textY, leadText, ui::FromLinear(ThemeAt(m_theme).accentWarm));
        y += rowHeight;
    }
    y += 12 * scale;

    // From the bottom up: the footer, the settings and the buttons; the moves take the space between.
    float bottom = panel.Bottom() - pad;
    const float footerHeight = p.LineHeight(FontId::Body);
    p.TextCentered(FontId::Body, {x, bottom - footerHeight, width, footerHeight}, T(Str::Footer), ui::Fade(MutedColor, 0.8f));
    bottom -= footerHeight + 14 * scale;

    const float rowHeight = 34 * scale, gap = 8 * scale;
    const float half = (width - gap) / 2;
    bottom -= rowHeight;
    if (Button(SoundButton, {x, bottom, half, rowHeight}, std::format("{}: {}", T(Str::Sound), T(m_sound.Enabled() ? Str::On : Str::Off))))
        ToggleSound();
    if (Button(LanguageButton, {x + half + gap, bottom, half, rowHeight}, T(Str::LanguageName)))
        ToggleLanguage();
    bottom -= gap;

    const std::array<std::string_view, 4> modes{T(Str::ModeWhite), T(Str::ModeBlack), T(Str::ModeTwoPlayers), T(Str::ModeDemo)};
    const Theme& theme = ThemeAt(m_theme);
    struct Choice {
        int id;
        Str label;
        std::string value;
    };
    const std::array<Choice, 3> choices{{
        {ThemeSelector, Str::Theme, std::string(m_language == Language::Russian ? theme.nameRussian : theme.nameEnglish)},
        {LevelSelector, Str::Level, std::format("{} · {}", m_level, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)))},
        {ModeSelector, Str::Mode, std::string(modes[static_cast<std::size_t>(m_mode)])},
    }};
    float labelWidth = 0;
    for (const Choice& choice : choices)
        labelWidth = std::max(labelWidth, p.Measure(FontId::Body, T(choice.label)));
    for (const Choice& choice : choices) {
        bottom -= rowHeight;
        if (const int change = Selector(choice.id, {x, bottom, width, rowHeight}, T(choice.label), choice.value, labelWidth); change != 0) {
            if (choice.id == ThemeSelector)
                SetTheme(m_theme + change);
            else if (choice.id == LevelSelector)
                SetLevel(m_level + change);
            else
                SetMode(m_mode + change);
        }
        bottom -= 6 * scale;
    }
    bottom -= 8 * scale;

    const float buttonHeight = 38 * scale;
    bottom -= buttonHeight;
    if (Button(NewGameButton, {x, bottom, half, buttonHeight}, T(Str::NewGame)))
        NewGame();
    if (Button(FlipButton, {x + half + gap, bottom, half, buttonHeight}, T(Str::Flip)))
        m_camera.Flip();
    bottom -= gap + buttonHeight;
    const float third = (width - 2 * gap) / 3;
    if (Button(UndoButton, {x, bottom, third, buttonHeight}, T(Str::Undo), m_game.CanUndo()))
        Undo();
    if (Button(RedoButton, {x + third + gap, bottom, third, buttonHeight}, T(Str::Redo), m_game.CanRedo()))
        Redo();
    if (Button(HintButton, {x + 2 * (third + gap), bottom, third, buttonHeight}, T(Str::Hint), HumanToMove() && !m_hintThinking, true))
        RequestHint();
    bottom -= 16 * scale;

    // The moves, with their heading, where there is room for them.
    const float listTop = y + p.LineHeight(FontId::Body) + 6 * scale;
    if (bottom - listTop > 40 * scale) {
        p.Text(FontId::Body, x, y, T(Str::Moves), MutedColor);
        DrawMoveList({x, listTop, width, bottom - listTop});
    }
}

void App::DrawBanner()
{
    if (m_bannerFade <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const chess::GameStatus status = m_game.Status();
    const std::string_view title = status.reason == chess::EndReason::Checkmate ? T(Str::Checkmate)
                                   : status.reason == chess::EndReason::Stalemate ? T(Str::Stalemate)
                                                                                  : T(Str::Draw);
    const std::string_view detail = status.result == chess::GameResult::WhiteWins   ? T(Str::WhiteWins)
                                    : status.result == chess::GameResult::BlackWins ? T(Str::BlackWins)
                                    : status.reason == chess::EndReason::Repetition ? T(Str::ByRepetition)
                                    : status.reason == chess::EndReason::FiftyMoves ? T(Str::ByFiftyMoves)
                                    : status.reason == chess::EndReason::InsufficientMaterial ? T(Str::ByMaterial)
                                                                                              : std::string_view{};
    const Rect card = BannerCard();
    p.Shadow(card, 24 * scale, ui::Fade(Rgba(0, 0, 0, 120), m_bannerFade));
    p.Fill(card, 18 * scale, ui::Fade(Rgba(14, 16, 24, 150), m_bannerFade));
    p.Outline(card, 18 * scale, ui::Fade(ui::FromLinear(ThemeAt(m_theme).accent), 0.6f * m_bannerFade));
    p.TextCentered(FontId::Display, {card.x, card.y + 8 * scale, card.width, 48 * scale}, title, ui::Fade(TextColor, m_bannerFade));
    p.TextCentered(FontId::Body, {card.x, card.y + 54 * scale, card.width, 26 * scale}, detail, ui::Fade(MutedColor, m_bannerFade));
}

void App::DrawToasts()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect free = FreeArea();
    float y = static_cast<float>(m_height) - 40 * scale;
    for (auto toast = m_toasts.rbegin(); toast != m_toasts.rend(); ++toast) {
        const float fade = std::min({1.0f, toast->age * 6, (2.8f - toast->age) * 3});
        const float width = p.Measure(FontId::Body, toast->text) + 40 * scale, height = 36 * scale;
        const Rect pill{free.x + (free.width - width) / 2, y - height, width, height};
        p.Shadow(pill, 14 * scale, ui::Fade(Rgba(0, 0, 0, 110), fade));
        p.Fill(pill, height / 2, ui::Fade(Rgba(20, 22, 30, 225), fade));
        p.TextCentered(FontId::Body, pill, toast->text, ui::Fade(TextColor, fade));
        y -= height + 8 * scale;
    }
}

void App::DrawPromotion()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Color side = m_game.Current().SideToMove();
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 90));
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;

    const float tile = PromotionTileSize * scale, gap = PromotionGap * scale, pad = PromotionPad * scale;
    const Rect card = PromotionCard();
    DrawCard(card);
    p.TextCentered(FontId::Heading, {card.x, card.y + 12 * scale, card.width, 36 * scale}, T(Str::PromoteTo), TextColor);

    constexpr std::array<PieceType, 4> types{PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight};
    constexpr std::array<const char*, 4> keys{"Q", "R", "B", "N"};
    std::array<Rect, 4> tiles;
    std::optional<chess::Move> chosen;
    for (int i = 0; i < 4; ++i) {
        tiles[i] = {card.x + pad + static_cast<float>(i) * (tile + gap), card.y + pad + 36 * scale, tile, tile};
        if (Button(PromotionTile + i, tiles[i], ""))
            for (const chess::Move& move : *m_promotion)
                if (move.promotion == types[i])
                    chosen = move;
    }
    p.Flush();
    for (int i = 0; i < 4; ++i) {
        const float inset = 10 * scale;
        m_renderer->DrawPieceIcon(types[i], side, tiles[i].x + inset, tiles[i].y + inset, tiles[i].width - 2 * inset, m_time * 0.9f + static_cast<float>(i) * 0.6f);
    }
    for (int i = 0; i < 4; ++i)
        p.Text(FontId::Body, tiles[i].x + 8 * scale, tiles[i].y + 6 * scale, keys[i], MutedColor);
    if (chosen)
        PlayMove(*chosen);
}

// The card's glass is frosted over the scene; over the panel, where there is nothing to frost, it is darker instead.
void App::DrawCard(Rect card)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const bool overPanel = m_panelSlide > 0.001f && card.Right() > PanelRect().x;
    p.Shadow(card, 26 * scale, Rgba(0, 0, 0, 140));
    p.Fill(card, 20 * scale, Rgba(18, 20, 28, overPanel ? 235 : 175));
    p.Outline(card, 20 * scale, Rgba(255, 255, 255, 30));
}

void App::DrawHelp()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));

    const float rowHeight = HelpRowHeight(), keysWidth = HelpKeysWidth(), pad = HelpPad * scale;
    const Rect card = HelpCard();
    const ui::Color accent = ui::FromLinear(ThemeAt(m_theme).accent);
    DrawCard(card);
    p.Text(FontId::Heading, card.x + pad, card.y + pad - 4 * scale, T(Str::Controls), TextColor);
    float y = card.y + pad + 44 * scale;
    for (const HelpLine& line : HelpLines()) {
        p.Text(FontId::Body, card.x + pad, y, line.keys, accent);
        p.Text(FontId::Body, card.x + pad + keysWidth, y, T(line.action), TextColor);
        y += rowHeight;
    }
    if (Button(HelpClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}

void App::DrawAbout()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));
    const Rect card = AboutCard();
    const float pad = AboutPad * scale;
    const ui::Color accent = ui::FromLinear(ThemeAt(m_theme).accent);
    DrawCard(card);

    // The icon, and beside it the name and the version.
    const float x = card.x + pad, width = card.width - 2 * pad, icon = AboutIcon * scale;
    float y = card.y + pad;
    p.Icon({x, y, icon, icon});
    const float titleHeight = p.LineHeight(FontId::Display), versionHeight = p.LineHeight(FontId::Body);
    const float titleY = y + (icon - titleHeight - versionHeight) / 2;
    p.Text(FontId::Display, x + icon + 18 * scale, titleY, "3D Chess", TextColor);
    p.Text(FontId::Body, x + icon + 18 * scale, titleY + titleHeight, "Version " CHESS3D_VERSION " for Linux", MutedColor);
    y += icon + 20 * scale;

    for (const std::string& line : WrapText(FontId::Body, AboutDescription, width)) {
        p.Text(FontId::Body, x, y, line, TextColor);
        y += p.LineHeight(FontId::Body) + 3 * scale;
    }
    y += 16 * scale;
    p.Text(FontId::Heading, x, y, "Created by Mykhailo Makarov", TextColor);
    y += p.LineHeight(FontId::Heading) + 2 * scale;
    p.Text(FontId::Body, x, y, "© 2026 · MIT License", MutedColor);
    y += p.LineHeight(FontId::Body) + 20 * scale;

    // The author's pages, opened in the browser.
    float nameWidth = 0;
    for (const Link& link : AuthorLinks)
        nameWidth = std::max(nameWidth, p.Measure(FontId::Body, link.name));
    for (std::size_t i = 0; i < AuthorLinks.size(); ++i) {
        const Link& link = AuthorLinks[i];
        const Rect row{x, y, width, LinkRow * scale};
        const Interaction state = Interact(AboutLinks + static_cast<int>(i), row, true);
        p.Fill(row, 9 * scale, state.pressed ? Rgba(255, 255, 255, 52) : state.hover ? Rgba(255, 255, 255, 34) : Rgba(255, 255, 255, 12));
        const float textY = row.y + (row.height - p.LineHeight(FontId::Body)) / 2;
        p.Text(FontId::Body, row.x + 14 * scale, textY, link.name, TextColor);
        p.Text(FontId::Body, row.x + 32 * scale + nameWidth, textY, link.address, accent);
        p.Text(FontId::Body, row.Right() - 14 * scale - p.Measure(FontId::Body, "↗"), textY, "↗", state.hover ? TextColor : MutedColor);
        if (state.clicked)
            OpenLink(link.url);
        y += (LinkRow + LinkGap) * scale;
    }

    if (Button(AboutClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}

