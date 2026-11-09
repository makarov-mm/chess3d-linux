#pragma once

#include "BoardView.h"
#include "Camera.h"
#include "Chess.h"
#include "Engine.h"
#include "Gl.h"
#include "Renderer.h"
#include "Resources.h"
#include "Settings.h"
#include "Sound.h"
#include "Strings.h"
#include "Text.h"
#include "UiPainter.h"

#include <SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The game's window: it runs the game, the computer players and the interface, and draws them.
class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();

private:
    enum Mode { PlayWhite, PlayBlack, TwoPlayers, Demo };

    // SDL from the first to the last moment of the game; everything else lives within it.
    struct SdlSession {
        SdlSession();
        ~SdlSession();
        SdlSession(const SdlSession&) = delete;
        SdlSession& operator=(const SdlSession&) = delete;
    };

    struct WindowDeleter {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };
    struct CursorDeleter {
        void operator()(SDL_Cursor* cursor) const { SDL_FreeCursor(cursor); }
    };

    // What the mouse did since the last frame, in pixels.
    struct Pointer {
        float x = -1, y = -1;
        bool leftDown = false, rightDown = false;
        bool leftPressed = false, leftReleased = false, rightPressed = false, rightReleased = false;
        float wheel = 0;
        bool consumed = false; // taken by the interface
    };

    enum class Gesture { None, PressPiece, DragPiece, TurnCamera };

    // The card shown over the game, which takes the mouse while it is open.
    enum class Dialog { None, Help, About };

    // What the pointer does to a widget in this frame.
    struct Interaction {
        bool hover = false, pressed = false, clicked = false;
    };

    struct Toast {
        std::string text;
        float age = 0;
    };

    // The window's size when it is neither maximized nor full screen, in window units.
    struct WindowSize {
        int width = 0, height = 0;
    };

    // A line of the help: the keys, and what they do.
    struct HelpLine {
        std::string_view keys;
        Str action;
    };

    void CreateMainWindow(const resources::Image& icon);
    void HandleEvent(const SDL_Event& event);
    void HandleWindowEvent(const SDL_WindowEvent& event);
    void UpdateSize();
    void ShowLoading();
    void BuildFonts();
    void Frame();
    void Update(float seconds);
    bool NeedsAnimation() const;

    // The game.
    bool IsHuman(chess::Color side) const;
    bool HumanToMove() const;
    void NewGame();
    void PlayMove(chess::Move move);
    void Undo();
    void Redo();
    void SetMode(int mode);
    void SetLevel(int level);
    void SetTheme(int theme);
    void ToggleSound();
    void ToggleLanguage();
    void StopComputer();
    void UpdateComputer(float seconds);
    void UpdateHint();
    void RequestHint();
    void AfterPositionChange();
    void Announce(const chess::MoveRecord& record);
    void ClearSelection();
    void Select(int square);
    void TryMove(int from, int to);
    void CopyGame(bool fen);
    void PasteGame();
    void OpenLink(const char* url);
    void Save();

    // Input.
    void PointerAt(int windowX, int windowY);
    void HandleBoardPointer();
    void HandleKey(const SDL_KeyboardEvent& key);
    int SquareUnderPointer() const;
    math::Vec3 PointerOnPlane(float height) const;
    chess::Color ViewSide() const { return m_mode == PlayBlack ? chess::Color::Black : chess::Color::White; }

    // The interface.
    float Scale() const { return m_scale; }
    ui::Rect PanelRect() const;
    ui::Rect FreeArea() const;
    std::vector<render::FrostedPanel> FrostedPanels() const;
    // The text in lines no wider than `width`, broken at the spaces.
    std::vector<std::string> WrapText(ui::FontId font, std::string_view text, float width) const;
    std::array<HelpLine, 21> HelpLines() const;
    // The width of the keys' column: as laid out, or wider where the desktop's font needs it.
    float HelpKeysWidth() const;
    float HelpRowHeight() const;
    // A card of the size over the game: beside the panel, or over it where the window is too narrow for that.
    ui::Rect CenteredCard(float width, float height) const;
    ui::Rect HelpCard() const;
    ui::Rect AboutCard() const;
    ui::Rect PromotionCard() const;
    ui::Rect BannerCard() const;
    void DrawInterface();
    void DrawPanel();
    void DrawMoveList(ui::Rect area);
    void DrawStatus(ui::Rect area);
    void DrawBanner();
    void DrawPromotion();
    // The background of a card over the game.
    void DrawCard(ui::Rect card);
    void DrawHelp();
    void DrawAbout();
    void DrawToasts();
    void ToggleDialog(Dialog dialog);
    // A press outside the open card closes it, unless it is the click that opened it.
    bool PressedOutside(ui::Rect card) const;
    // Hovering, pressing and clicking; a click sounds.
    Interaction Interact(int id, ui::Rect rect, bool enabled);
    bool Button(int id, ui::Rect rect, std::string_view label, bool enabled = true, bool accent = false);
    // A label with a value stepped by two arrows, past the width of the longest label of the column; returns -1 or 1
    // when an arrow is clicked, else 0.
    int Selector(int id, ui::Rect rect, std::string_view label, std::string_view value, float labelWidth);
    void ShowToast(std::string text);
    std::string_view T(Str id) const { return Tr(id, m_language); }
    std::string PlayerName(chess::Color side) const;

    void ToggleFullScreen();

    // Declared in the order of their making; destroyed the other way round, the window after what draws in it.
    SdlSession m_sdl;
    SoundBank m_sound;
    std::unique_ptr<SDL_Window, WindowDeleter> m_window;
    std::unique_ptr<SDL_Cursor, CursorDeleter> m_arrowCursor, m_handCursor;
    std::unique_ptr<gl::Context> m_context;
    std::unique_ptr<render::Renderer> m_renderer;
    std::unique_ptr<ui::Painter> m_painter;
    std::array<std::unique_ptr<text::Font>, 3> m_fonts;
    std::unique_ptr<BoardView> m_view;

    Settings m_settings;
    Language m_language = Language::English;
    int m_width = 0, m_height = 0;             // of the picture, in pixels
    int m_windowWidth = 0, m_windowHeight = 0; // in window units, which the mouse reports
    float m_textScale = 1;                     // the desktop's
    float m_scale = 1;                         // of the interface: the desktop's, in pixels
    float m_fontScale = 0;                     // that the fonts were made for
    bool m_running = true;
    bool m_dirty = true; // something to draw since the last frame
    bool m_fullScreen = false;
    bool m_maximizedBeforeFullScreen = false;
    WindowSize m_normalSize, m_previousNormalSize;
    std::uint32_t m_normalSizeTime = 0; // when the window last took a size of its own, in SDL's milliseconds
    bool m_cursorHand = false, m_cursorShownHand = false;
    std::chrono::steady_clock::time_point m_lastFrame;
    float m_time = 0;

    chess::Game m_game;
    OrbitCamera m_camera;
    int m_mode = PlayWhite;
    int m_level = 3;
    int m_theme = 0;
    chess::AiPlayer m_computer{32};
    chess::AiPlayer m_hinter{16};
    bool m_computerThinking = false;
    float m_computerClock = 0;
    std::optional<chess::SearchResult> m_computerResult;
    bool m_hintThinking = false;
    std::optional<chess::Move> m_hint;
    float m_hintGlow = 0;
    bool m_announcedEnd = false;

    Pointer m_pointer;
    Gesture m_gesture = Gesture::None;
    float m_pressX = 0, m_pressY = 0;
    float m_dragX = 0, m_dragY = 0; // the pointer when the camera last turned
    int m_pressSquare = -1;
    int m_selected = -1;
    std::vector<chess::Move> m_selectedMoves;
    int m_hoverPiece = -1;
    int m_hoverSquare = -1;
    std::optional<std::vector<chess::Move>> m_promotion; // the choices, while the player picks one

    int m_activeWidget = 0;
    bool m_panelVisible = true;
    float m_panelSlide = 1;
    Dialog m_dialog = Dialog::None;
    bool m_dialogOpening = false; // opened in this frame
    float m_moveScroll = 0;
    bool m_followMoves = true;
    float m_bannerFade = 0;
    std::vector<Toast> m_toasts;
};
