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

// Cursor + pick state. g_held_sq is -1 when no piece is picked, otherwise
// the source square of the held piece. The cursor doesn't persist across
// chess-mode exits; it resets each time the player enters chess.
static int8_t   g_cursor_f = 4;
static int8_t   g_cursor_r = 0;
static int8_t   g_held_sq  = -1;

// Set while the Ctrl+R "reset game?" prompt is up. While true, handleKey
// consumes every press: `/` commits a fresh game, anything else dismisses.
static bool     g_confirm_reset = false;

// True while a CPU move is in flight. The render draws a static
// "thinking..." label in the side panel when set — no animation, since
// the presence of the label alone communicates state and animation would
// require extra state and a poll function that buys nothing.
static bool     g_cpu_thinking  = false;

static void (*g_redraw_cb)() = nullptr;

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

// Move-list capacity used everywhere a stack-allocated move array is
// needed. 218 is the theoretical max for any legal position; 256 gives
// headroom and aligns nicely.
static constexpr int MAX_MOVES = 256;

// Generate all pseudo-legal moves for the side to move (no king-safety
// filter applied — callers must verify with isLegal). Primary form
// writes into a caller-supplied fixed array, so search recursion stays
// off the heap.
static void generatePseudoLegal(const Position& p, Move* out, int& count) {
    count = 0;
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
                out[count++] = m;
                // Forward two from start rank.
                if (r == start_rank) {
                    int nr2 = r + 2 * dir;
                    if (p.board[idx(f, nr2)] == EMPTY)
                        out[count++] = { (int8_t)s, (int8_t)idx(f, nr2), 0, 0 };
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
                    out[count++] = m;
                } else if (t == p.ep) {
                    out[count++] = { (int8_t)s, (int8_t)t, 0, 1 };
                }
            }
        }
        else if (kind == KNIGHT) {
            for (auto& off : KNIGHT_OFF) {
                int nf = f + off[0], nr = r + off[1];
                if (!onBoard(nf, nr)) continue;
                int t = idx(nf, nr);
                if (p.board[t] == EMPTY || !sameSide(pc, p.board[t]))
                    out[count++] = { (int8_t)s, (int8_t)t, 0, 0 };
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
                        out[count++] = { (int8_t)s, (int8_t)t, 0, 0 };
                    } else {
                        if (!sameSide(pc, tp))
                            out[count++] = { (int8_t)s, (int8_t)t, 0, 0 };
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
                    out[count++] = { (int8_t)s, (int8_t)t, 0, 0 };
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
                    out[count++] = { (int8_t)idx(4, home), (int8_t)idx(6, home), 0, 2 };
                }
                if (can_q
                    && p.board[idx(1, home)] == EMPTY
                    && p.board[idx(2, home)] == EMPTY
                    && p.board[idx(3, home)] == EMPTY
                    && !isAttacked(p, idx(4, home), opp)
                    && !isAttacked(p, idx(3, home), opp)
                    && !isAttacked(p, idx(2, home), opp)) {
                    out[count++] = { (int8_t)idx(4, home), (int8_t)idx(2, home), 0, 2 };
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

static void generateLegal(Position& p, Move* out, int& count) {
    Move pseudo[MAX_MOVES];
    int pseudo_n;
    generatePseudoLegal(p, pseudo, pseudo_n);
    count = 0;
    int side = p.side;
    for (int i = 0; i < pseudo_n; ++i) {
        Undo u;
        makeMove(p, pseudo[i], u);
        if (!isInCheck(p, side)) out[count++] = pseudo[i];
        unmakeMove(p, u);
    }
}

// std::vector wrapper for the handful of cold-path callers (input
// handling, result detection). Search uses the fixed-array form
// directly to keep nodes off the heap.
static void generateLegal(Position& p, std::vector<Move>& out) {
    Move buf[MAX_MOVES];
    int n;
    generateLegal(p, buf, n);
    out.assign(buf, buf + n);
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

// --- Search: evaluation ---------------------------------------------------
//
// Score from white's POV in centipawns. Material values plus
// piece-square tables (PSTs). PSTs are indexed by (rank*8 + file) from
// white's POV; black pieces look up the vertically mirrored square so
// the same table serves both sides.

static constexpr int PIECE_VALUE[7] = { 0, 100, 320, 330, 500, 900, 20000 };

static const int8_t PST_PAWN[64] = {
     0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0, 0, 0,
     5, 5, 0,-5,-5, 0, 5, 5,
     0, 0, 5,10,10, 5, 0, 0,
     5, 5,10,25,25,10, 5, 5,
    10,10,20,30,30,20,10,10,
    50,50,50,50,50,50,50,50,
     0, 0, 0, 0, 0, 0, 0, 0,
};
static const int8_t PST_KNIGHT[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50,
};
static const int8_t PST_BISHOP[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -20,-10,-10,-10,-10,-10,-10,-20,
};
static const int8_t PST_ROOK[64] = {
      0,  0,  0,  5,  5,  0,  0,  0,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      5, 10, 10, 10, 10, 10, 10,  5,
      0,  0,  0,  0,  0,  0,  0,  0,
};
static const int8_t PST_QUEEN[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -10,  5,  5,  5,  5,  5,  0,-10,
      0,  0,  5,  5,  5,  5,  0, -5,
     -5,  0,  5,  5,  5,  5,  0, -5,
    -10,  0,  5,  5,  5,  5,  0,-10,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20,
};
static const int8_t PST_KING[64] = {
     20, 30, 10,  0,  0, 10, 30, 20,
     20, 20,  0,  0,  0,  0, 20, 20,
    -10,-20,-20,-20,-20,-20,-20,-10,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
};
static const int8_t* PST_BY_KIND[7] = {
    nullptr, PST_PAWN, PST_KNIGHT, PST_BISHOP, PST_ROOK, PST_QUEEN, PST_KING
};

static int evaluate(const Position& p) {
    int score = 0;
    for (int s = 0; s < 64; ++s) {
        int8_t pc = p.board[s];
        if (pc == EMPTY) continue;
        int kind = pieceKind(pc);
        int sq   = (pc > 0) ? s : idx(fileOf(s), 7 - rankOf(s));
        int term = PIECE_VALUE[kind] + PST_BY_KIND[kind][sq];
        score += (pc > 0) ? term : -term;
    }
    return score;
}

// --- Search: alpha-beta with iterative deepening + quiescence -----------
//
// Iterative deepening drives a negamax alpha-beta search out to a
// time budget. Move ordering is MVV-LVA for captures plus the
// previous iteration's principal-variation move first at the root.
// Move lists for each ply live in a single static stack so node
// expansion never touches the heap.

static constexpr int      MAX_PLY                 = 24;   // depth 8 + quiescence headroom
static constexpr int      SEARCH_BUDGET_MS        = 5000;

// Per-difficulty engine parameters. Indexed by Difficulty enum. Easy is
// intentionally shallow + randomised so games stay varied; Hard matches
// the unconstrained engine.
struct LevelParams { int max_depth; int top_n; bool quiescence; };
static const LevelParams LEVELS[3] = {
    { 2, 3, false }, // EASY
    { 4, 1, true  }, // MEDIUM
    { 8, 1, true  }, // HARD
};
static constexpr int TOPN_TIE_EPS = 50;  // centipawn slack defining "tied for best" at the root
static Difficulty g_difficulty = HARD;

static const char* levelLabelStr(Difficulty d) {
    switch (d) {
        case EASY:   return "Easy";
        case MEDIUM: return "Medium";
        case HARD:   return "Hard";
    }
    return "?";
}

static void cycleDifficultyForward() {
    Difficulty next = (g_difficulty == HARD) ? EASY
                     : (Difficulty)((int)g_difficulty + 1);
    setDifficulty(next);
}
static bool       g_use_quiescence = true;  // set by searchBestMove from current level
static constexpr uint32_t DEADLINE_CHECK_INTERVAL = 1024; // must be power of two
static constexpr int      MATE_SCORE              = 30000;
static constexpr int      INF_SCORE               = 1 << 30;

static Move     g_move_stack[MAX_PLY][MAX_MOVES];
static uint32_t g_search_deadline_ms = 0;
static uint64_t g_search_nodes       = 0;
static bool     g_search_aborted     = false;

static inline void pollDeadline() {
    if ((g_search_nodes & (DEADLINE_CHECK_INTERVAL - 1)) == 0) {
        if (millis() >= g_search_deadline_ms) g_search_aborted = true;
    }
}

// MVV-LVA score: 16 * victim − attacker. Quiet moves score 0.
static int mvvLva(const Position& p, const Move& m) {
    int8_t victim = p.board[m.to];
    if (m.flag == 1) victim = (p.side > 0) ? -PAWN : PAWN;  // en passant
    if (victim == EMPTY) return 0;
    int v_kind = pieceKind(victim);
    int a_kind = pieceKind(p.board[m.from]);
    return 16 * PIECE_VALUE[v_kind] - PIECE_VALUE[a_kind];
}

// Selection-sort one slot at a time. A move matching the PV hint wins
// outright. Cheaper than a full sort when alpha-beta cuts after a few
// trials.
static void selectBestNext(const Position& p, Move* moves, int count, int start,
                           int pv_from, int pv_to) {
    int best_i = start;
    int best_score = -1;
    for (int i = start; i < count; ++i) {
        int s = mvvLva(p, moves[i]);
        if (moves[i].from == pv_from && moves[i].to == pv_to) s = INF_SCORE;
        if (s > best_score) { best_score = s; best_i = i; }
    }
    if (best_i != start) std::swap(moves[start], moves[best_i]);
}

static int quiesce(Position& p, int alpha, int beta, int ply) {
    ++g_search_nodes;
    pollDeadline();
    if (g_search_aborted) return 0;

    int stand_pat = (p.side > 0) ? evaluate(p) : -evaluate(p);
    if (stand_pat >= beta) return beta;
    if (stand_pat > alpha) alpha = stand_pat;
    if (ply >= MAX_PLY - 1) return alpha;

    Move* moves = g_move_stack[ply];
    int count;
    generatePseudoLegal(p, moves, count);

    int side = p.side;
    for (int i = 0; i < count; ++i) {
        selectBestNext(p, moves, count, i, -1, -1);
        const Move& m = moves[i];
        bool is_capture = (p.board[m.to] != EMPTY) || (m.flag == 1);
        if (!is_capture) continue;
        Undo u;
        makeMove(p, m, u);
        if (isInCheck(p, side)) { unmakeMove(p, u); continue; }
        int score = -quiesce(p, -beta, -alpha, ply + 1);
        unmakeMove(p, u);
        if (g_search_aborted) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int negamax(Position& p, int depth, int alpha, int beta, int ply,
                   int pv_from, int pv_to) {
    if (depth == 0) {
        if (g_use_quiescence) return quiesce(p, alpha, beta, ply);
        return (p.side > 0) ? evaluate(p) : -evaluate(p);
    }

    ++g_search_nodes;
    pollDeadline();
    if (g_search_aborted) return 0;
    if (ply >= MAX_PLY - 1) return (p.side > 0) ? evaluate(p) : -evaluate(p);

    Move* moves = g_move_stack[ply];
    int count;
    generatePseudoLegal(p, moves, count);

    int side = p.side;
    int legal_moves = 0;
    int best = -INF_SCORE;

    for (int i = 0; i < count; ++i) {
        selectBestNext(p, moves, count, i, pv_from, pv_to);
        const Move& m = moves[i];
        Undo u;
        makeMove(p, m, u);
        if (isInCheck(p, side)) { unmakeMove(p, u); continue; }
        ++legal_moves;
        int score = -negamax(p, depth - 1, -beta, -alpha, ply + 1, -1, -1);
        unmakeMove(p, u);
        if (g_search_aborted) return 0;
        if (score > best) best = score;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }

    if (legal_moves == 0) {
        // Mate or stalemate. Mate score includes ply so shorter mates
        // outscore longer when winning, longer mates outscore shorter
        // when losing.
        if (isInCheck(p, side)) return -MATE_SCORE + ply;
        return 0;
    }
    return best;
}

static Move searchBestMove(Position& p) {
    const LevelParams& lvl = LEVELS[g_difficulty];
    g_use_quiescence     = lvl.quiescence;
    g_search_deadline_ms = millis() + SEARCH_BUDGET_MS;
    g_search_aborted     = false;
    g_search_nodes       = 0;

    Move root_moves [MAX_MOVES];   // shuffled by selectBestNext during search
    Move last_moves [MAX_MOVES];   // snapshot of root order at end of last completed iteration
    int  last_scores[MAX_MOVES];
    int  root_count;
    generateLegal(p, root_moves, root_count);
    Move best{ -1, -1, 0, 0 };
    if (root_count == 0) return best;
    best = root_moves[0];

    int pv_from = -1, pv_to = -1;
    int last_complete_count = 0;

    for (int depth = 1; depth <= lvl.max_depth; ++depth) {
        int alpha = -INF_SCORE;
        int beta  =  INF_SCORE;
        Move iter_best  = root_moves[0];
        int  iter_score = -INF_SCORE;
        int  iter_scores[MAX_MOVES];

        for (int i = 0; i < root_count; ++i) {
            selectBestNext(p, root_moves, root_count, i, pv_from, pv_to);
            const Move& m = root_moves[i];
            Undo u;
            makeMove(p, m, u);
            int score = -negamax(p, depth - 1, -beta, -alpha, 1, -1, -1);
            unmakeMove(p, u);
            if (g_search_aborted) break;
            iter_scores[i] = score;
            if (score > iter_score) {
                iter_score = score;
                iter_best  = m;
            }
            if (iter_score > alpha) alpha = iter_score;
        }

        if (g_search_aborted) break;
        best    = iter_best;
        pv_from = best.from;
        pv_to   = best.to;
        for (int i = 0; i < root_count; ++i) {
            last_moves [i] = root_moves[i];
            last_scores[i] = iter_scores[i];
        }
        last_complete_count = root_count;
        if (root_count == 1) break;
        if (iter_score >= MATE_SCORE - 100) break;  // forced mate found
    }

    // Top-N randomisation for easy levels. Candidates are root moves
    // whose score is within TOPN_TIE_EPS of the best from the last
    // completed iteration. Capped at the level's top_n. Non-mate-bound
    // only — we still play a forced mate deterministically.
    if (lvl.top_n > 1 && last_complete_count > 1) {
        int best_score = -INF_SCORE;
        for (int i = 0; i < last_complete_count; ++i)
            if (last_scores[i] > best_score) best_score = last_scores[i];
        if (best_score < MATE_SCORE - 100) {
            int cand_idx[MAX_MOVES];
            int n_cand = 0;
            for (int i = 0; i < last_complete_count && n_cand < lvl.top_n; ++i)
                if (last_scores[i] >= best_score - TOPN_TIE_EPS)
                    cand_idx[n_cand++] = i;
            if (n_cand > 1) {
                int pick = esp_random() % n_cand;
                best = last_moves[cand_idx[pick]];
            }
        }
    }

    return best;
}

// --- CPU opponent --------------------------------------------------------

static void cpuMove() {
    g_cpu_thinking = true;
    if (g_redraw_cb) g_redraw_cb();
    Move m = searchBestMove(g_pos);
    g_cpu_thinking = false;
    if (m.from < 0) return;  // no legal moves — caller's updateResult tail handles game-over.
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
    prefs.putChar ("diff",   (char)g_difficulty);
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
    // Difficulty is independent of the rest of position state — load it
    // regardless, defaulting to HARD on missing key (matches existing
    // installs' implicit behaviour from before this setting existed).
    int8_t d = (int8_t)prefs.getChar("diff", (char)HARD);
    if (d < EASY || d > HARD) d = HARD;
    g_difficulty = (Difficulty)d;
    prefs.end();
    return ok;
}

static void newGame() {
    setupStart(g_pos);
    g_held_sq  = -1;
    g_cursor_f = 4;
    g_cursor_r = 0;
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
// Panel snugs up to the board with a 2 px gap. Rank/file labels were
// removed, so the panel no longer needs to clear the rank-digit column.
static constexpr int PANEL_X  = BOARD_X + BOARD_PX + 2;

// Colours in 16-bit RGB565.
static constexpr uint16_t COL_LIGHT  = 0xA514;  // darker warm beige — converges toward COL_DARK while staying clearly lighter
static constexpr uint16_t COL_DARK   = 0x7BCE;  // lighter warm brown
static constexpr uint16_t COL_WHITE  = 0xFFFF;
static constexpr uint16_t COL_BLACK  = 0x0000;
static constexpr uint16_t COL_PANEL  = 0x10A2;  // dark slate
static constexpr uint16_t COL_TXT    = 0xFFFF;
static constexpr uint16_t COL_DIM    = 0x8410;
static constexpr uint16_t COL_PICK   = 0x041F;  // blue — selector colour while no piece is held (pick mode)
static constexpr uint16_t COL_PLACE  = 0x07FF;  // cyan — selector colour while a piece is held (place mode)
static constexpr uint16_t COL_HELD   = 0xFFE0;  // yellow — outline on the picked-up square

// 12 × 12 mono sprites per piece kind. Each row is the leftmost 12 bits
// of a uint16 (bit 11 = leftmost pixel). One sprite per kind serves both
// sides — render colours the set bits in white or black to taste.
// Indexed [kind - 1] with kind from the PAWN..KING enum.
static const uint16_t PIECE_SPRITES[6][12] = {
    // Pawn
    { 0x000, 0x0F0, 0x0F0, 0x1F8, 0x0F0, 0x0F0,
      0x0F0, 0x1F8, 0x3FC, 0x7FE, 0xFFF, 0x000 },
    // Knight
    { 0x0F0, 0x1F8, 0x3FC, 0x5FC, 0x7FC, 0x7FC,
      0x0FC, 0x1FE, 0x3FF, 0x3FF, 0x7FF, 0x000 },
    // Bishop
    { 0x040, 0x0E0, 0x0A0, 0x0E0, 0x0E0, 0x1F0,
      0x1F0, 0x0E0, 0x0E0, 0x1F0, 0x3F8, 0x7FC },
    // Rook
    { 0xDB6, 0xDB6, 0xFFF, 0x7FE, 0x3FC, 0x3FC,
      0x3FC, 0x3FC, 0x3FC, 0x7FE, 0xFFF, 0x000 },
    // Queen
    { 0x6DB, 0x6DB, 0x3FC, 0x2F4, 0x1F8, 0x1F8,
      0x1F8, 0x1F8, 0x3FC, 0x7FE, 0xFFF, 0x000 },
    // King
    { 0x040, 0x0E0, 0x1F0, 0x040, 0x3FC, 0x2F4,
      0x1F8, 0x1F8, 0x1F8, 0x3FC, 0x7FE, 0x000 },
};

static void drawPieceSprite(M5Canvas& d, int x, int y, int8_t pc) {
    const uint16_t* sprite = PIECE_SPRITES[pieceKind(pc) - 1];
    uint16_t fg = isWhite(pc) ? COL_WHITE : COL_BLACK;
    // 12 × 12 sprite centred in a 16 × 16 square (2 px margin all round).
    for (int sy = 0; sy < 12; ++sy) {
        uint16_t row = sprite[sy];
        for (int sx = 0; sx < 12; ++sx) {
            if (row & (1u << (11 - sx))) d.drawPixel(x + 2 + sx, y + 2 + sy, fg);
        }
    }
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
            if (pc != EMPTY) drawPieceSprite(d, x, y, pc);
        }
    }

    // Held-piece outline (yellow, 2 px) — matches the cursor's dimensions
    // so the two read as the same kind of mark. Drawn first so the cursor
    // overlays it when both land on the same square.
    if (g_held_sq >= 0) {
        int hf = fileOf(g_held_sq), hr = rankOf(g_held_sq);
        int hx = BOARD_X + hf * SQUARE;
        int hy = BOARD_Y + (7 - hr) * SQUARE;
        d.drawRect(hx,     hy,     SQUARE,     SQUARE,     COL_HELD);
        d.drawRect(hx + 1, hy + 1, SQUARE - 2, SQUARE - 2, COL_HELD);
    }

    // Cursor outline (2 px). Blue while picking; cyan once a piece is
    // held (placing) — the colour change is the user's cue that another
    // press will commit a move rather than select a piece.
    {
        uint16_t col = (g_held_sq >= 0) ? COL_PLACE : COL_PICK;
        int cx = BOARD_X + g_cursor_f * SQUARE;
        int cy = BOARD_Y + (7 - g_cursor_r) * SQUARE;
        d.drawRect(cx,     cy,     SQUARE,     SQUARE,     col);
        d.drawRect(cx + 1, cy + 1, SQUARE - 2, SQUARE - 2, col);
    }

    // Side panel.
    d.fillRect(PANEL_X, 0, 240 - PANEL_X, 135, COL_PANEL);
    d.setFont(&fonts::Font0);
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

    if (g_cpu_thinking) {
        // Static text, right-aligned next to the CHESS label. "thinking"
        // is 8 chars at 6 px each = 48 px; fits inside the panel width.
        d.setTextColor(COL_TXT, COL_PANEL);
        d.setCursor(240 - 8 * 6 - 1, 4);
        d.print("thinking");
    }

    label(18, "level");
    value(28, levelLabelStr(g_difficulty));

    label(42, "last");
    value(52, g_last_move);
    label(66, "state");
    value(76, g_held_sq >= 0 ? "place" : "pick");

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
    d.print("esc:exit tab:lvl");

    // Reset-game confirmation modal. Drawn last so it sits on top of the
    // board and side panel. Yellow frame reuses the held-piece warning hue.
    if (g_confirm_reset) {
        constexpr int MODAL_W = 210;
        constexpr int MODAL_H = 64;
        int mx = (240 - MODAL_W) / 2;
        int my = (135 - MODAL_H) / 2;
        d.fillRect(mx, my, MODAL_W, MODAL_H, COL_BLACK);
        d.drawRect(mx,     my,     MODAL_W,     MODAL_H,     COL_HELD);
        d.drawRect(mx + 1, my + 1, MODAL_W - 2, MODAL_H - 2, COL_HELD);

        d.setTextSize(2);
        auto centreSize2 = [&](const char* s, int row_y, uint16_t fg) {
            d.setTextColor(fg, COL_BLACK);
            int w = (int)strlen(s) * 12;
            d.setCursor((240 - w) / 2, row_y);
            d.print(s);
        };
        centreSize2("RESET GAME?",     my + 10, COL_TXT);
        centreSize2("return = confirm", my + 38, COL_DIM);
    }
}

// --- Move execution ------------------------------------------------------

// Attempt a from→to move with the current side. Returns true and updates
// g_last_move on success; returns false silently on illegal target.
static bool tryMakeMove(int from, int to) {
    std::vector<Move> legal;
    generateLegal(g_pos, legal);
    for (const Move& m : legal) {
        if (m.from == from && m.to == to) {
            Undo u;
            makeMove(g_pos, m, u);
            std::snprintf(g_last_move, sizeof(g_last_move), "%c%d%c%d",
                          'a' + fileOf(from), 1 + rankOf(from),
                          'a' + fileOf(to),   1 + rankOf(to));
            return true;
        }
    }
    return false;
}

// --- Input handling ------------------------------------------------------

// Handle a select / place press at the current cursor square. Pick rules:
// - Nothing held + cursor on own piece → pick.
// - Held + cursor on the held square → drop (deselect).
// - Held + cursor on another own piece → switch selection.
// - Held + cursor on legal target → execute move and let the CPU reply.
// Illegal targets are a silent no-op (held state stays so the user can
// try a different square without re-picking).
static void onSelectAtCursor() {
    int cur_sq = idx(g_cursor_f, g_cursor_r);
    int8_t pc  = g_pos.board[cur_sq];
    int side   = g_pos.side;
    bool own   = (pc != EMPTY) && ((side > 0) == (pc > 0));

    if (g_held_sq < 0) {
        if (own) g_held_sq = cur_sq;
        return;
    }
    if (g_held_sq == cur_sq) { g_held_sq = -1; return; }
    if (own) { g_held_sq = cur_sq; return; }

    if (tryMakeMove(g_held_sq, cur_sq)) {
        g_held_sq = -1;
        updateResult();
        if (!g_result[0]) {
            g_cpu_thinking = true;
            if (g_redraw_cb) g_redraw_cb();
            cpuMove();
            g_cpu_thinking = false;
            updateResult();
        }
        save();
    }
}

void setRedrawCallback(void (*cb)()) { g_redraw_cb = cb; }

bool handleKey(const Keyboard_Class::KeysState& state) {
    // Reset-game confirmation. `/` commits; any user input cancels;
    // pure modifier holds (no key in `word`, no del/enter) leave the
    // prompt up. Return true unconditionally so esc dismisses the
    // prompt rather than exiting chess.
    if (g_confirm_reset) {
        bool confirmed = state.enter;
        bool any_input = state.enter || state.del || !state.word.empty();
        if (confirmed) {
            newGame();
            g_confirm_reset = false;
        } else if (any_input) {
            g_confirm_reset = false;
        }
        return true;
    }

    // Ctrl+R opens the reset-game prompt.
    if (state.ctrl) {
        for (char c : state.word) {
            if (c == 'R' || c == 'r') {
                g_confirm_reset = true;
                return true;
            }
        }
    }

    // Fn+` = esc — explicit exit. State preserved; caller pops the mode.
    if (state.fn) {
        for (char c : state.word) if (c == '`') return false;
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

    // Backspace cancels a pick-up (no-op if nothing held).
    if (state.del) {
        g_held_sq = -1;
        return true;
    }

    // Enter picks / places / re-picks based on what's under the cursor.
    if (state.enter) {
        onSelectAtCursor();
        return true;
    }

    // Tab cycles difficulty Easy → Medium → Hard → Easy. Same call path
    // as the Settings row, so the new value persists immediately.
    if (state.tab) {
        cycleDifficultyForward();
        return true;
    }

    // Pure modifier press (e.g. just shift held) — stay, no action.
    if (state.word.empty()) return true;

    for (char c : state.word) {
        switch (c) {
            case ';': if (g_cursor_r < 7) g_cursor_r++; break;
            case '.': if (g_cursor_r > 0) g_cursor_r--; break;
            case ',': if (g_cursor_f > 0) g_cursor_f--; break;
            case '/': if (g_cursor_f < 7) g_cursor_f++; break;
            // Anything else — letter, digit, symbol — exits chess.
            default: return false;
        }
    }
    return true;
}

// --- Public interface ----------------------------------------------------

void initAtBoot() {
    if (!load()) setupStart(g_pos);
    g_held_sq  = -1;
    g_cursor_f = 4;
    g_cursor_r = 0;
    g_result    = "";
    updateResult();
}

void enter() {
    // Fresh ergonomics each time the player opens chess: cursor at e1,
    // no piece held. State (board, last move) persists.
    g_held_sq  = -1;
    g_cursor_f = 4;
    g_cursor_r = 0;
    g_active = true;
}

void exit() {
    g_active = false;
    save();
}

bool active() { return g_active; }

Difficulty getDifficulty() { return g_difficulty; }

void setDifficulty(Difficulty d) {
    if (d < EASY || d > HARD) return;
    if (d == g_difficulty) return;
    g_difficulty = d;
    save();
}

}  // namespace chess
