/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm> // For std::min
#include <cassert>
#include <cstring>   // For std::memset

#include "material.h"
#include "thread.h"

using namespace std;

namespace {

  // Polynomial material imbalance parameters

  const int QuadraticOurs[][PIECE_TYPE_NB] = {
    //            OUR PIECES
    // pair pawn knight bishop rook queen
    {1667                               }, // Bishop pair
    {  40,    2                         }, // Pawn
    {  32,  255,  -3                    }, // Knight      OUR PIECES
    {   0,  104,   4,    0              }, // Bishop
    { -26,   -2,  47,   105,  -149      }, // Rook
    {-185,   24, 122,   137,  -134,   0 }  // Queen
  };

  const int QuadraticTheirs[][PIECE_TYPE_NB] = {
    //           THEIR PIECES
    // pair pawn knight bishop rook queen
    {   0                               }, // Bishop pair
    {  36,    0                         }, // Pawn
    {   9,   63,   0                    }, // Knight      OUR PIECES
    {  59,   65,  42,     0             }, // Bishop
    {  46,   39,  24,   -24,    0       }, // Rook
    { 101,  100, -37,   141,  268,    0 }  // Queen
  };

  const int QuadraticOursAnti[][PIECE_TYPE_NB] = {
    //            OUR PIECES
    // pair pawn knight bishop rook queen king
    {  -62                                    }, // Bishop pair
    { -179,  59                               }, // Pawn
    {  -50,  178,  -47                        }, // Knight      OUR PIECES
    {    0, -130, -187,    0                  }, // Bishop
    { -155, -317,   60, -218, -288            }, // Rook
    {   89, -259,  -60, -179,  -32, -76       }, // Queen
    { -217,  -79,   40,  -23,    9, -63, -197 }  // King

  };

  const int QuadraticTheirsAnti[][PIECE_TYPE_NB] = {
    //           THEIR PIECES
    // pair pawn knight bishop rook queen king
    {    0                                   }, // Bishop pair
    {  110,    0                             }, // Pawn
    {    9,   60,    0                       }, // Knight      OUR PIECES
    {  -53, -143,   33,    0                 }, // Bishop
    {   73, -298,    3,   41,   0            }, // Rook
    { -141, -370,   56,   45, -79,   0       }, // Queen
    {  246,  -40, -194,  178, -39,  74,  0   }  // King
  };

  // Endgame evaluation and scaling functions are accessed directly and not through
  // the function maps because they correspond to more than one material hash key.
  Endgame<CHESS_VARIANT, KXK>    EvaluateKXK[] = { Endgame<CHESS_VARIANT, KXK>(WHITE),    Endgame<CHESS_VARIANT, KXK>(BLACK) };

  Endgame<CHESS_VARIANT, KBPsK>  ScaleKBPsK[]  = { Endgame<CHESS_VARIANT, KBPsK>(WHITE),  Endgame<CHESS_VARIANT, KBPsK>(BLACK) };
  Endgame<CHESS_VARIANT, KQKRPs> ScaleKQKRPs[] = { Endgame<CHESS_VARIANT, KQKRPs>(WHITE), Endgame<CHESS_VARIANT, KQKRPs>(BLACK) };
  Endgame<CHESS_VARIANT, KPsK>   ScaleKPsK[]   = { Endgame<CHESS_VARIANT, KPsK>(WHITE),   Endgame<CHESS_VARIANT, KPsK>(BLACK) };
  Endgame<CHESS_VARIANT, KPKP>   ScaleKPKP[]   = { Endgame<CHESS_VARIANT, KPKP>(WHITE),   Endgame<CHESS_VARIANT, KPKP>(BLACK) };

  // Helper used to detect a given material distribution
  bool is_KXK(const Position& pos, Color us) {
    return  !more_than_one(pos.pieces(~us))
          && pos.non_pawn_material(us) >= RookValueMg;
  }

  bool is_KBPsKs(const Position& pos, Color us) {
    return   pos.non_pawn_material(us) == BishopValueMg
          && pos.count<BISHOP>(us) == 1
          && pos.count<PAWN  >(us) >= 1;
  }

  bool is_KQKRPs(const Position& pos, Color us) {
    return  !pos.count<PAWN>(us)
          && pos.non_pawn_material(us) == QueenValueMg
          && pos.count<QUEEN>(us)  == 1
          && pos.count<ROOK>(~us) == 1
          && pos.count<PAWN>(~us) >= 1;
  }

  /// imbalance() calculates the imbalance by comparing the piece count of each
  /// piece type for both colors.
  template<Color Us>
  int imbalance(const Position& pos, const int pieceCount[][PIECE_TYPE_NB]) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    int bonus = 0;

    // Second-degree polynomial material imbalance by Tord Romstad
#ifdef ANTI
    for (int pt1 = NO_PIECE_TYPE; pt1 <= (pos.is_anti() ? KING : QUEEN); ++pt1)
#else
    for (int pt1 = NO_PIECE_TYPE; pt1 <= QUEEN; ++pt1)
#endif
    {
        if (!pieceCount[Us][pt1])
            continue;

        int v = 0;

        for (int pt2 = NO_PIECE_TYPE; pt2 <= pt1; ++pt2)
#ifdef ANTI
            if (pos.is_anti())
                v +=  QuadraticOursAnti[pt1][pt2] * pieceCount[Us][pt2]
                    + QuadraticTheirsAnti[pt1][pt2] * pieceCount[Them][pt2];
            else
#endif
            v +=  QuadraticOurs[pt1][pt2] * pieceCount[Us][pt2]
                + QuadraticTheirs[pt1][pt2] * pieceCount[Them][pt2];

        bonus += pieceCount[Us][pt1] * v;
    }

    return bonus;
  }

} // namespace

namespace Material {

/// Material::probe() looks up the current position's material configuration in
/// the material hash table. It returns a pointer to the Entry if the position
/// is found. Otherwise a new Entry is computed and stored there, so we don't
/// have to recompute all when the same material configuration occurs again.

Entry* probe(const Position& pos) {

  Key key = pos.material_key() ^ pos.variant();
  Entry* e = pos.this_thread()->materialTable[key];

  if (e->key == key)
      return e;

  std::memset(e, 0, sizeof(Entry));
  e->key = key;
  e->factor[WHITE] = e->factor[BLACK] = (uint8_t)SCALE_FACTOR_NORMAL;
  e->gamePhase = pos.game_phase();

  // Let's look if we have a specialized evaluation function for this particular
  // material configuration. Firstly we look for a fixed configuration one, then
  // for a generic one if the previous search failed.
  if ((e->evaluationFunction = pos.this_thread()->endgames.probe<Value>(key)) != nullptr)
      return e;

  for (Color c = WHITE; c <= BLACK; ++c)
      if (is_KXK(pos, c))
      {
          e->evaluationFunction = &EvaluateKXK[c];
          return e;
      }

  // OK, we didn't find any special evaluation function for the current material
  // configuration. Is there a suitable specialized scaling function?
  EndgameBase<ScaleFactor>* sf;

  if ((sf = pos.this_thread()->endgames.probe<ScaleFactor>(key)) != nullptr)
  {
      e->scalingFunction[sf->strong_side()] = sf; // Only strong color assigned
      return e;
  }

  // We didn't find any specialized scaling function, so fall back on generic
  // ones that refer to more than one material distribution. Note that in this
  // case we don't return after setting the function.
  for (Color c = WHITE; c <= BLACK; ++c)
  {
    if (is_KBPsKs(pos, c))
        e->scalingFunction[c] = &ScaleKBPsK[c];

    else if (is_KQKRPs(pos, c))
        e->scalingFunction[c] = &ScaleKQKRPs[c];
  }

  Value npm_w = pos.non_pawn_material(WHITE);
  Value npm_b = pos.non_pawn_material(BLACK);

  if (npm_w + npm_b == VALUE_ZERO && pos.pieces(PAWN)) // Only pawns on the board
  {
      if (!pos.count<PAWN>(BLACK))
      {
          assert(pos.variant() != CHESS_VARIANT || pos.count<PAWN>(WHITE) >= 2);

          e->scalingFunction[WHITE] = &ScaleKPsK[WHITE];
      }
      else if (!pos.count<PAWN>(WHITE))
      {
          assert(pos.variant() != CHESS_VARIANT || pos.count<PAWN>(BLACK) >= 2);

          e->scalingFunction[BLACK] = &ScaleKPsK[BLACK];
      }
      else if (pos.count<PAWN>(WHITE) == 1 && pos.count<PAWN>(BLACK) == 1)
      {
          // This is a special case because we set scaling functions
          // for both colors instead of only one.
          e->scalingFunction[WHITE] = &ScaleKPKP[WHITE];
          e->scalingFunction[BLACK] = &ScaleKPKP[BLACK];
      }
  }

  // Zero or just one pawn makes it difficult to win, even with a small material
  // advantage. This catches some trivial draws like KK, KBK and KNK and gives a
  // drawish scale factor for cases such as KRKBP and KmmKm (except for KBBKN).
  if (!pos.count<PAWN>(WHITE) && npm_w - npm_b <= BishopValueMg)
      e->factor[WHITE] = uint8_t(npm_w <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_b <= BishopValueMg ? 4 : 14);

  if (!pos.count<PAWN>(BLACK) && npm_b - npm_w <= BishopValueMg)
      e->factor[BLACK] = uint8_t(npm_b <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_w <= BishopValueMg ? 4 : 14);

  if (pos.count<PAWN>(WHITE) == 1 && npm_w - npm_b <= BishopValueMg)
      e->factor[WHITE] = (uint8_t) SCALE_FACTOR_ONEPAWN;

  if (pos.count<PAWN>(BLACK) == 1 && npm_b - npm_w <= BishopValueMg)
      e->factor[BLACK] = (uint8_t) SCALE_FACTOR_ONEPAWN;

  // Evaluate the material imbalance. We use PIECE_TYPE_NONE as a place holder
  // for the bishop pair "extended piece", which allows us to be more flexible
  // in defining bishop pair bonuses.
  const int PieceCount[COLOR_NB][PIECE_TYPE_NB] = {
  { pos.count<BISHOP>(WHITE) > 1, pos.count<PAWN>(WHITE), pos.count<KNIGHT>(WHITE),
    pos.count<BISHOP>(WHITE)    , pos.count<ROOK>(WHITE), pos.count<QUEEN >(WHITE) },
  { pos.count<BISHOP>(BLACK) > 1, pos.count<PAWN>(BLACK), pos.count<KNIGHT>(BLACK),
    pos.count<BISHOP>(BLACK)    , pos.count<ROOK>(BLACK), pos.count<QUEEN >(BLACK) } };

  e->value = int16_t((imbalance<WHITE>(pos, PieceCount) - imbalance<BLACK>(pos, PieceCount)) / 16);
  return e;
}

} // namespace Material
