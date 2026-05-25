// Chess game module — see chess.h for the public interface and module
// rationale. This file holds the engine, the renderer, and the input
// handler. Pieces are encoded as signed integers (positive = white,
// negative = black) on a 64-entry board indexed rank*8 + file with rank 0
// at white's back rank.

#include "chess.h"

#include <Preferences.h>
#include <esp_random.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace chess {

// --- Piece encoding -------------------------------------------------------

enum : int8_t {
    EMPTY  = 0,
    PAWN   = 1,
    KNIGHT = 2,
    BISHOP = 3,
    ROOK   = 4,
    QUEEN  = 5,
    KING   = 6,
};

static inline int8_t pieceKind(int8_t p)  { return p < 0 ? -p : p; }
static inline bool   isWhite(int8_t p)    { return p > 0; }
static inline bool   isBlack(int8_t p)    { return p < 0; }
static inline bool   sameSide(int8_t a, int8_t b) {
    return (a > 0 && b > 0) || (a < 0 && b < 0);
}

static inline int idx(int file, int rank) { return rank * 8 + file; }
static inline int fileOf(int s)          { return s & 7; }
static inline int rankOf(int s)          { return s >> 3; }
static inline bool onBoard(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

// --- Game state -----------------------------------------------------------

struct Move {
    int8_t from;
    int8_t to;
    int8_t promotion;   // piece kind (always QUEEN in this PoC), or 0
    int8_t flag;        // 0 = normal, 1 = en passant, 2 = castle
};

struct Position {
    int8_t board[64];
    int8_t side;            // +1 white to move, -1 black to move
    uint8_t castle;         // bit0 WK, bit1 WQ, bit2 BK, bit3 BQ
    int8_t  ep;             // en passant target square, -1 if none
};

static Position g_pos;
static bool     g_active = false;

// Input buffer for the player's move ("e2e4" notation).
static char     g_input[5] = {0,0,0,0,0};
static int      g_input_len = 0;

// Last move played, for the side panel. "—" if none yet.
static char     g_last_move[8] = "-";

// Game result message ("", "WHITE WINS", "BLACK WINS", "DRAW").
static const char* g_result = "";

// --- Initial position -----------------------------------------------------

static void setupStart(Position& p) {
    static const int8_t back[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
    for (int i = 0; i < 64; ++i) p.board[i] = EMPTY;
    for (int f = 0; f < 8; ++f) {
        p.board[idx(f, 0)] =  back[f];
        p.board[idx(f, 1)] =  PAWN;
        p.board[idx(f, 6)] = -PAWN;
        p.board[idx(f, 7)] = -back[f];
    }
    p.side   = +1;
    p.castle = 0x0F;
    p.ep     = -1;
}

// --- Move generation ------------------------------------------------------

static const int KNIGHT_OFF[8][2] = {
    {+1,+2},{+2,+1},{+2,-1},{+1,-2},{-1,-2},{-2,-1},{-2,+1},{-1,+2}
};
static const int KING_OFF[8][2] = {
    {+1,0},{+1,+1},{0,+1},{-1,+1},{-1,0},{-1,-1},{0,-1},{+1,-1}
};
static const int BISHOP_DIR[4][2] = { {+1,+1},{-1,+1},{-1,-1},{+1,-1} };
static const int ROOK_DIR  [4][2] = { {+1,0},{0,+1},{-1,0},{0,-1} };

static bool isAttacked(const Position& p, int target, int by_side);

// Generate all pseudo-legal moves for the side to move (no king-safety
// filter applied — callers must verify with isLegal).
static void generatePseudoLegal(const Position& p, std::vector<Move>& out) {
    out.clear();
    int side = p.side;
    for (int s = 0; s < 64; ++s) {
        int8_t pc = p.board[s];
        if (pc == EMPTY) continue;
        if ((side > 0) != (pc > 0)) continue;
        int kind = pieceKind(pc);
        int f = fileOf(s), r = rankOf(s);

        if (kind == PAWN) {
            int dir = (side > 0) ? +1 : -1;
            int start_rank = (side > 0) ? 1 : 6;
            int promo_rank = (side > 0) ? 7 : 0;
            // Forward one.
            int nr = r + dir;
            if (onBoard(f, nr) && p.board[idx(f, nr)] == EMPTY) {
                Move m{ (int8_t)s, (int8_t)idx(f, nr), 0, 0 };
                if (nr == promo_rank) m.promotion = QUEEN;
                out.push_back(m);
                // Forward two from start rank.
                if (r == start_rank) {
                    int nr2 = r + 2 * dir;
                    if (p.board[idx(f, nr2)] == EMPTY)
                        out.push_back({ (int8_t)s, (int8_t)idx(f, nr2), 0, 0 });
                }
            }
            // Captures.
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (!onBoard(nf, nr)) continue;
                int t = idx(nf, nr);
                int8_t tp = p.board[t];
                if (tp != EMPTY && !sameSide(pc, tp)) {
                    Move m{ (int8_t)s, (int8_t)t, 0, 0 };
                    if (nr == promo_rank) m.promotion = QUEEN;
                    out.push_back(m);
                } else if (t == p.ep) {
                    out.push_back({ (int8_t)s, (int8_t)t, 0, 1 });
                }
            }
        }
        else if (kind == KNIGHT) {
            for (auto& off : KNIGHT_OFF) {
                int nf = f + off[0], nr = r + off[1];
                if (!onBoard(nf, nr)) continue;
                int t = idx(nf, nr);
                if (p.board[t] == EMPTY || !sameSide(pc, p.board[t]))
                    out.push_back({ (int8_t)s, (int8_t)t, 0, 0 });
            }
        }
        else if (kind == BISHOP || kind == ROOK || kind == QUEEN) {
            const int (*dirs)[2];
            int n_dirs;
            if      (kind == BISHOP) { dirs = BISHOP_DIR; n_dirs = 4; }
            else if (kind == ROOK)   { dirs = ROOK_DIR;   n_dirs = 4; }
            else                     { dirs = BISHOP_DIR; n_dirs = 8; }  // queen: walks both arrays
            for (int i = 0; i < n_dirs; ++i) {
                int df, dr;
                if (kind == QUEEN && i >= 4) { df = ROOK_DIR[i-4][0]; dr = ROOK_DIR[i-4][1]; }
                else                          { df = dirs[i][0];      dr = dirs[i][1]; }
                int nf = f + df, nr = r + dr;
                while (onBoard(nf, nr)) {
                    int t = idx(nf, nr);
                    int8_t tp = p.board[t];
                    if (tp == EMPTY) {
                        out.push_back({ (int8_t)s, (int8_t)t, 0, 0 });
                    } else {
                        if (!sameSide(pc, tp))
                            out.push_back({ (int8_t)s, (int8_t)t, 0, 0 });
                        break;
                    }
                    nf += df; nr += dr;
                }
            }
        }
        else if (kind == KING) {
            for (auto& off : KING_OFF) {
                int nf = f + off[0], nr = r + off[1];
                if (!onBoard(nf, nr)) continue;
                int t = idx(nf, nr);
                if (p.board[t] == EMPTY || !sameSide(pc, p.board[t]))
                    out.push_back({ (int8_t)s, (int8_t)t, 0, 0 });
            }
            // Castling: king & rook unmoved, squares between empty, king
            // not in/through/into check.
            int home = (side > 0) ? 0 : 7;
            if (r == home && f == 4) {
                int opp = -side;
                bool can_k = (side > 0) ? (p.castle & 0x01) : (p.castle & 0x04);
                bool can_q = (side > 0) ? (p.castle & 0x02) : (p.castle & 0x08);
                if (can_k
                    && p.board[idx(5, home)] == EMPTY
                    && p.board[idx(6, home)] == EMPTY
                    && !isAttacked(p, idx(4, home), opp)
                    && !isAttacked(p, idx(5, home), opp)
                    && !isAttacked(p, idx(6, home), opp)) {
                    out.push_back({ (int8_t)idx(4, home), (int8_t)idx(6, home), 0, 2 });
                }
                if (can_q
                    && p.board[idx(1, home)] == EMPTY
                    && p.board[idx(2, home)] == EMPTY
                    && p.board[idx(3, home)] == EMPTY
                    && !isAttacked(p, idx(4, home), opp)
                    && !isAttacked(p, idx(3, home), opp)
                    && !isAttacked(p, idx(2, home), opp)) {
                    out.push_back({ (int8_t)idx(4, home), (int8_t)idx(2, home), 0, 2 });
                }
            }
        }
    }
}

// True if `target` is attacked by any piece of `by_side` (+1 or -1).
// Implemented directly rather than via generatePseudoLegal to avoid
// recursion into castling (which itself asks about attacked squares).
static bool isAttacked(const Position& p, int target, int by_side) {
    int tf = fileOf(target), tr = rankOf(target);
    // Pawn attacks come from one rank toward `by_side`'s home.
    int pawn_dir = (by_side > 0) ? +1 : -1;
    for (int df = -1; df <= 1; df += 2) {
        int f = tf + df, r = tr - pawn_dir;
        if (onBoard(f, r)) {
            int8_t pc = p.board[idx(f, r)];
            if (pc != EMPTY && pieceKind(pc) == PAWN
                && ((by_side > 0) == (pc > 0))) return true;
        }
    }
    // Knight.
    for (auto& off : KNIGHT_OFF) {
        int f = tf + off[0], r = tr + off[1];
        if (!onBoard(f, r)) continue;
        int8_t pc = p.board[idx(f, r)];
        if (pc != EMPTY && pieceKind(pc) == KNIGHT
            && ((by_side > 0) == (pc > 0))) return true;
    }
    // King (adjacent).
    for (auto& off : KING_OFF) {
        int f = tf + off[0], r = tr + off[1];
        if (!onBoard(f, r)) continue;
        int8_t pc = p.board[idx(f, r)];
        if (pc != EMPTY && pieceKind(pc) == KING
            && ((by_side > 0) == (pc > 0))) return true;
    }
    // Sliding: bishops/queens on diagonals, rooks/queens on orthogonals.
    for (int i = 0; i < 4; ++i) {
        int df = BISHOP_DIR[i][0], dr = BISHOP_DIR[i][1];
        int f = tf + df, r = tr + dr;
        while (onBoard(f, r)) {
            int8_t pc = p.board[idx(f, r)];
            if (pc != EMPTY) {
                int k = pieceKind(pc);
                if ((k == BISHOP || k == QUEEN) && ((by_side > 0) == (pc > 0))) return true;
                break;
            }
            f += df; r += dr;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int df = ROOK_DIR[i][0], dr = ROOK_DIR[i][1];
        int f = tf + df, r = tr + dr;
        while (onBoard(f, r)) {
            int8_t pc = p.board[idx(f, r)];
            if (pc != EMPTY) {
                int k = pieceKind(pc);
                if ((k == ROOK || k == QUEEN) && ((by_side > 0) == (pc > 0))) return true;
                break;
            }
            f += df; r += dr;
        }
    }
    return false;
}

// --- Make / unmake --------------------------------------------------------

struct Undo {
    Move    move;
    int8_t  captured;
    int8_t  ep_before;
    uint8_t castle_before;
    int8_t  side_before;
};

static int findKing(const Position& p, int side) {
    for (int s = 0; s < 64; ++s) {
        int8_t pc = p.board[s];
        if (pieceKind(pc) == KING && ((side > 0) == (pc > 0))) return s;
    }
    return -1;
}

static void makeMove(Position& p, const Move& m, Undo& u) {
    u.move          = m;
    u.captured      = p.board[m.to];
    u.ep_before     = p.ep;
    u.castle_before = p.castle;
    u.side_before   = p.side;

    int8_t mover = p.board[m.from];
    p.board[m.from] = EMPTY;
    p.board[m.to]   = mover;
    p.ep            = -1;

    if (m.flag == 1) {
        // En passant: captured pawn sits on the from-rank, to-file.
        int cap_sq = idx(fileOf(m.to), rankOf(m.from));
        u.captured = p.board[cap_sq];
        p.board[cap_sq] = EMPTY;
    } else if (m.flag == 2) {
        // Castle: move the rook to its post-castle square.
        int home = rankOf(m.from);
        if (fileOf(m.to) == 6) {
            p.board[idx(5, home)] = p.board[idx(7, home)];
            p.board[idx(7, home)] = EMPTY;
        } else {
            p.board[idx(3, home)] = p.board[idx(0, home)];
            p.board[idx(0, home)] = EMPTY;
        }
    }

    // Promotion.
    if (m.promotion) {
        p.board[m.to] = (mover > 0) ? m.promotion : -m.promotion;
    }

    // Pawn double-push sets ep target on the in-between square.
    if (pieceKind(mover) == PAWN && std::abs(rankOf(m.to) - rankOf(m.from)) == 2) {
        p.ep = idx(fileOf(m.from), (rankOf(m.from) + rankOf(m.to)) / 2);
    }

    // Strip castling rights when king or rook moves, or rook is captured.
    auto stripFromSquare = [&](int s) {
        if (s == idx(4, 0)) p.castle &= ~0x03;
        if (s == idx(4, 7)) p.castle &= ~0x0C;
        if (s == idx(7, 0)) p.castle &= ~0x01;
        if (s == idx(0, 0)) p.castle &= ~0x02;
        if (s == idx(7, 7)) p.castle &= ~0x04;
        if (s == idx(0, 7)) p.castle &= ~0x08;
    };
    stripFromSquare(m.from);
    stripFromSquare(m.to);

    p.side = -p.side;
}

static void unmakeMove(Position& p, const Undo& u) {
    const Move& m = u.move;
    p.side   = u.side_before;
    p.ep     = u.ep_before;
    p.castle = u.castle_before;

    int8_t mover = p.board[m.to];
    if (m.promotion) mover = (mover > 0) ? PAWN : -PAWN;
    p.board[m.from] = mover;
    p.board[m.to]   = EMPTY;

    if (m.flag == 1) {
        int cap_sq = idx(fileOf(m.to), rankOf(m.from));
        p.board[cap_sq] = u.captured;
    } else {
        p.board[m.to] = u.captured;
    }

    if (m.flag == 2) {
        int home = rankOf(m.from);
        if (fileOf(m.to) == 6) {
            p.board[idx(7, home)] = p.board[idx(5, home)];
            p.board[idx(5, home)] = EMPTY;
        } else {
            p.board[idx(0, home)] = p.board[idx(3, home)];
            p.board[idx(3, home)] = EMPTY;
        }
    }
}

static bool isInCheck(const Position& p, int side) {
    int k = findKing(p, side);
    if (k < 0) return false;
    return isAttacked(p, k, -side);
}

static void generateLegal(Position& p, std::vector<Move>& out) {
    std::vector<Move> pseudo;
    generatePseudoLegal(p, pseudo);
    out.clear();
    int side = p.side;
    for (const Move& m : pseudo) {
        Undo u;
        makeMove(p, m, u);
        if (!isInCheck(p, side)) out.push_back(m);
        unmakeMove(p, u);
    }
}

// --- Game-result detection ------------------------------------------------

static void updateResult() {
    std::vector<Move> legal;
    generateLegal(g_pos, legal);
    if (!legal.empty()) { g_result = ""; return; }
    if (isInCheck(g_pos, g_pos.side)) {
        g_result = (g_pos.side > 0) ? "BLACK WINS" : "WHITE WINS";
    } else {
        g_result = "DRAW";
    }
}

// --- CPU opponent (random legal — PoC) -----------------------------------

static void cpuMove() {
    std::vector<Move> legal;
    generateLegal(g_pos, legal);
    if (legal.empty()) return;
    uint32_t pick = esp_random() % legal.size();
    Move m = legal[pick];
    Undo u;
    makeMove(g_pos, m, u);
    std::snprintf(g_last_move, sizeof(g_last_move), "%c%d%c%d",
                  'a' + fileOf(m.from), 1 + rankOf(m.from),
                  'a' + fileOf(m.to),   1 + rankOf(m.to));
}

// --- Persistence ---------------------------------------------------------

static const char* PREFS_NS = "chess";

static void save() {
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, false)) return;
    prefs.putBytes("board", g_pos.board, 64);
    prefs.putChar ("side",   (char)g_pos.side);
    prefs.putUChar("castle", g_pos.castle);
    prefs.putChar ("ep",     (char)g_pos.ep);
    prefs.putString("last",  g_last_move);
    prefs.end();
}

static bool load() {
    Preferences prefs;
    if (!prefs.begin(PREFS_NS, true)) return false;
    bool ok = false;
    if (prefs.isKey("board")) {
        size_t n = prefs.getBytes("board", g_pos.board, 64);
        if (n == 64) {
            g_pos.side   = (int8_t)prefs.getChar ("side",   +1);
            g_pos.castle = prefs.getUChar("castle", 0x0F);
            g_pos.ep     = (int8_t)prefs.getChar ("ep",     -1);
            String s = prefs.getString("last", "-");
            std::strncpy(g_last_move, s.c_str(), sizeof(g_last_move) - 1);
            g_last_move[sizeof(g_last_move) - 1] = 0;
            ok = true;
        }
    }
    prefs.end();
    return ok;
}

static void newGame() {
    setupStart(g_pos);
    g_input_len = 0;
    g_input[0]  = 0;
    std::strcpy(g_last_move, "-");
    g_result    = "";
    save();
}

// --- Renderer ------------------------------------------------------------

// Layout: 16 px squares, 128 px board, top-left at (4, 4). Right panel
// from x = 138 to x = 240 carries status text.
static constexpr int BOARD_X = 4;
static constexpr int BOARD_Y = 4;
static constexpr int SQUARE  = 16;
static constexpr int BOARD_PX = 8 * SQUARE;       // 128
static constexpr int PANEL_X  = BOARD_X + BOARD_PX + 6;  // 138

// Colours in 16-bit RGB565.
static constexpr uint16_t COL_LIGHT  = 0xCE99;  // warm off-white
static constexpr uint16_t COL_DARK   = 0x6B0C;  // medium brown
static constexpr uint16_t COL_WHITE  = 0xFFFF;
static constexpr uint16_t COL_BLACK  = 0x0000;
static constexpr uint16_t COL_PANEL  = 0x10A2;  // dark slate
static constexpr uint16_t COL_TXT    = 0xFFFF;
static constexpr uint16_t COL_DIM    = 0x8410;

static char pieceGlyph(int8_t pc) {
    static const char W[] = " PNBRQK";
    int k = pieceKind(pc);
    if (k == 0) return ' ';
    return isWhite(pc) ? W[k] : (char)(W[k] + ('a' - 'A'));
}

void render(M5Canvas& canvas) {
    auto& d = canvas;
    d.fillScreen(COL_BLACK);

    // Squares + pieces. Rank 0 (white) goes at the bottom so the visual
    // matches white-at-bottom convention.
    for (int r = 0; r < 8; ++r) {
        for (int f = 0; f < 8; ++f) {
            int x = BOARD_X + f * SQUARE;
            int y = BOARD_Y + (7 - r) * SQUARE;
            bool light = ((f + r) & 1) != 0;
            d.fillRect(x, y, SQUARE, SQUARE, light ? COL_LIGHT : COL_DARK);
            int8_t pc = g_pos.board[idx(f, r)];
            if (pc != EMPTY) {
                char g = pieceGlyph(pc);
                d.setFont(&fonts::Font0);
                d.setTextSize(2);
                d.setTextColor(isWhite(pc) ? COL_WHITE : COL_BLACK,
                               light ? COL_LIGHT : COL_DARK);
                d.setCursor(x + (SQUARE - 12) / 2, y + (SQUARE - 16) / 2);
                d.print(g);
            }
        }
    }

    // File / rank labels along the bottom + left edges of the board.
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(COL_DIM, COL_BLACK);
    for (int f = 0; f < 8; ++f) {
        d.setCursor(BOARD_X + f * SQUARE + SQUARE / 2 - 2,
                    BOARD_Y + BOARD_PX + 1);
        d.print((char)('a' + f));
    }
    for (int r = 0; r < 8; ++r) {
        d.setCursor(BOARD_X + BOARD_PX + 1,
                    BOARD_Y + (7 - r) * SQUARE + SQUARE / 2 - 4);
        d.print(1 + r);
    }

    // Side panel.
    d.fillRect(PANEL_X, 0, 240 - PANEL_X, 135, COL_PANEL);
    d.setTextSize(1);

    auto label = [&](int y, const char* k) {
        d.setTextColor(COL_DIM, COL_PANEL);
        d.setCursor(PANEL_X + 3, y);
        d.print(k);
    };
    auto value = [&](int y, const char* v) {
        d.setTextColor(COL_TXT, COL_PANEL);
        d.setCursor(PANEL_X + 3, y);
        d.print(v);
    };

    label(4,  "CHESS");
    label(18, "turn");
    value(28, g_pos.side > 0 ? "white" : "black");
    label(42, "last");
    value(52, g_last_move);
    label(66, "input");
    value(76, g_input_len ? g_input : "...");

    if (g_result && g_result[0]) {
        d.setTextColor(COL_TXT, COL_PANEL);
        d.setCursor(PANEL_X + 3, 92);
        d.print(g_result);
        d.setTextColor(COL_DIM, COL_PANEL);
        d.setCursor(PANEL_X + 3, 102);
        d.print("n=new");
    } else if (isInCheck(g_pos, g_pos.side)) {
        d.setTextColor(COL_TXT, COL_PANEL);
        d.setCursor(PANEL_X + 3, 92);
        d.print("CHECK");
    }

    // Footer hint.
    d.setTextColor(COL_DIM, COL_PANEL);
    d.setCursor(PANEL_X + 3, 122);
    d.print("esc:exit");
}

// --- Move parsing --------------------------------------------------------

static bool parseAndPlay(const char* in) {
    if (std::strlen(in) != 4) return false;
    int ff = in[0] - 'a', fr = in[1] - '1';
    int tf = in[2] - 'a', tr = in[3] - '1';
    if (!onBoard(ff, fr) || !onBoard(tf, tr)) return false;
    int from = idx(ff, fr), to = idx(tf, tr);
    std::vector<Move> legal;
    generateLegal(g_pos, legal);
    for (const Move& m : legal) {
        if (m.from == from && m.to == to) {
            Undo u;
            makeMove(g_pos, m, u);
            std::snprintf(g_last_move, sizeof(g_last_move),
                          "%c%d%c%d", in[0], in[1]-'0', in[2], in[3]-'0');
            return true;
        }
    }
    return false;
}

// --- Input handling ------------------------------------------------------

bool handleKey(const Keyboard_Class::KeysState& state) {
    // Fn+` = esc — explicit exit. State preserved; caller pops the mode.
    if (state.fn) {
        for (char c : state.word) if (c == '`') return false;
        // Other Fn combos are not ours — let the host process them and
        // exit chess. We do NOT consume.
        return state.word.empty();  // pure modifier press: stay
    }

    // Game over: 'n' starts a fresh game; any other key exits.
    if (g_result && g_result[0]) {
        for (char c : state.word) {
            if (c == 'n' || c == 'N') { newGame(); return true; }
            return false;
        }
        return state.word.empty();
    }

    // Pure modifier press (e.g. just shift) — stay but don't process.
    if (state.word.empty() && !state.del) return true;

    if (state.del) {
        if (g_input_len > 0) { g_input_len--; g_input[g_input_len] = 0; }
        return true;
    }

    for (char c : state.word) {
        bool is_file = (c >= 'a' && c <= 'h');
        bool is_rank = (c >= '1' && c <= '8');
        // Position 0 and 2 want a file; 1 and 3 want a rank.
        bool want_file = (g_input_len % 2) == 0;
        if (want_file ? !is_file : !is_rank) {
            // Not a chess input character — exit mode.
            return false;
        }
        g_input[g_input_len++] = c;
        g_input[g_input_len]   = 0;
        if (g_input_len == 4) {
            if (parseAndPlay(g_input)) {
                g_input_len = 0; g_input[0] = 0;
                updateResult();
                if (!g_result[0]) {
                    cpuMove();
                    updateResult();
                }
                save();
            } else {
                // Illegal: clear the buffer so the user can try again.
                g_input_len = 0; g_input[0] = 0;
            }
        }
    }
    return true;
}

// --- Public interface ----------------------------------------------------

void initAtBoot() {
    if (!load()) setupStart(g_pos);
    g_input_len = 0;
    g_input[0]  = 0;
    g_result    = "";
    updateResult();
}

void enter() {
    g_active = true;
}

void exit() {
    g_active = false;
    save();
}

bool active() { return g_active; }

}  // namespace chess
