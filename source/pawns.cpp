/*
  SugaR, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  SugaR is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  SugaR is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>

#include "bitboard.h"
#include "pawns.h"
#include "position.h"
#include "thread.h"

#define PAWN_SCORES

namespace {

  #define V Value
  #define S(mg, eg) make_score(mg, eg)

  // Isolated pawn penalty
  constexpr Score Isolated = S(13, 16);

  // Backward pawn penalty
  constexpr Score Backward = S(17, 11);
  
#ifdef PAWN_SCORES
	//  Pawn Scores Isolated in Rank 3
	constexpr Score PawnScoresIsolatedRank3 = S(-05, +00);

	//  Pawn Scores Connected Passed
	constexpr Score PawnScoresConnectedPassed = S(-16, +16);
	constexpr Score KingSafetyCompensationPawnScoresConnectedPassed = S(-05, +00);
	//	Protected Passed Pawn
	constexpr Score ProtectedPassedPawn = S(+05, +05);
#endif

  // Connected pawn bonus by opposed, phalanx, #support and rank
  Score Connected[2][2][3][RANK_NB];

  // Doubled pawn penalty
  constexpr Score Doubled = S(13, 40);

  // Strength of pawn shelter for our king by [distance from edge][rank].
  // RANK_1 = 0 is used for files where we have no pawn, or pawn is behind our king.
  constexpr Value ShelterStrength[int(FILE_NB) / 2][RANK_NB] = {
    { V( 7), V(76), V(84), V( 38), V( 7), V( 30), V(-19) },
				 
    { V(-3), V(93), V(52), V(-17), V(12), V(-22), V(-35) },
				
    { V(-6), V(83), V(25), V(-24), V(15), V( 22), V(-39) },
				 
				 
    { V(11), V(83), V(19), V(  8), V(18), V(-21), V(-30) }
  };

  // Danger of enemy pawns moving toward our king by [type][distance from edge][rank].
  // For the unblocked case, RANK_1 = 0 is used when opponent has no pawn on the
  // given file, or their pawn is behind our king.
  constexpr Value StormDanger[][4][RANK_NB] = {
    { { V(25),  V( 79), V(107), V( 51), V( 27) },  // UnBlocked
      { V(15),  V( 45), V(131), V(  8), V( 25) },
      { V( 0),  V( 42), V(118), V( 56), V( 27) },
      { V( 3),  V( 54), V(110), V( 55), V( 26) } },
    { { V( 0),  V(  0), V( 37), V(  5), V(-48) },  // BlockedByPawn
      { V( 0),  V(  0), V( 68), V(-12), V( 13) },
      { V( 0),  V(  0), V(111), V(-25), V( -3) },
      { V( 0),  V(  0), V(108), V( 14), V( 21) } }
  };

  #undef S
  #undef V

  template<Color Us>
  Score evaluate(const Position& pos, Pawns::Entry* e) {

    constexpr Color     Them = (Us == WHITE ? BLACK : WHITE);
    constexpr Direction Up   = (Us == WHITE ? NORTH : SOUTH);

    Bitboard b, neighbours, stoppers, doubled, supported, phalanx;
    Bitboard lever, leverPush;
    Square s;
    bool opposed, backward;
    Score score = SCORE_ZERO;
    const Square* pl = pos.squares<PAWN>(Us);

    Bitboard ourPawns   = pos.pieces(  Us, PAWN);
    Bitboard theirPawns = pos.pieces(Them, PAWN);

    e->passedPawns[Us] = e->pawnAttacksSpan[Us] = e->weakUnopposed[Us] = 0;
    e->semiopenFiles[Us] = 0xFF;
    e->kingSquares[Us]   = SQ_NONE;
    e->pawnAttacks[Us]   = pawn_attacks_bb<Us>(ourPawns);
    e->pawnsOnSquares[Us][BLACK] = popcount(ourPawns & DarkSquares);
    e->pawnsOnSquares[Us][WHITE] = pos.count<PAWN>(Us) - e->pawnsOnSquares[Us][BLACK];

    // Loop through all pawns of the current color and score each pawn
    while ((s = *pl++) != SQ_NONE)
    {
        assert(pos.piece_on(s) == make_piece(Us, PAWN));

        File f = file_of(s);

        e->semiopenFiles[Us]   &= ~(1 << f);
        e->pawnAttacksSpan[Us] |= pawn_attack_span(Us, s);

        // Flag the pawn
        opposed    = theirPawns & forward_file_bb(Us, s);
        stoppers   = theirPawns & passed_pawn_mask(Us, s);
        lever      = theirPawns & PawnAttacks[Us][s];
        leverPush  = theirPawns & PawnAttacks[Us][s + Up];
        doubled    = ourPawns   & (s - Up);
        neighbours = ourPawns   & adjacent_files_bb(f);
        phalanx    = neighbours & rank_bb(s);
        supported  = neighbours & rank_bb(s - Up);

        // A pawn is backward when it is behind all pawns of the same color
        // on the adjacent files and cannot be safely advanced.
        backward =  !(ourPawns & pawn_attack_span(Them, s + Up))
                  && (stoppers & (leverPush | (s + Up)));

        // Passed pawns will be properly scored in evaluation because we need
        // full attack info to evaluate them. Include also not passed pawns
        // which could become passed after one or two pawn pushes when are
        // not attacked more times than defended.
        if (   !(stoppers ^ lever ^ leverPush)
            && !(ourPawns & forward_file_bb(Us, s))
            && popcount(supported) >= popcount(lever) - 1
            && popcount(phalanx)   >= popcount(leverPush))
            e->passedPawns[Us] |= s;

        else if (   stoppers == SquareBB[s + Up]
                 && relative_rank(Us, s) >= RANK_5)
        {
            b = shift<Up>(supported) & ~theirPawns;
            while (b)
                if (!more_than_one(theirPawns & PawnAttacks[Us][pop_lsb(&b)]))
                    e->passedPawns[Us] |= s;
        }

        // Score this pawn
        if (supported | phalanx)
            score += Connected[opposed][bool(phalanx)][popcount(supported)][relative_rank(Us, s)];

        else if (!neighbours)
			{
				score -= Isolated, e->weakUnopposed[Us] += !opposed;

#ifdef PAWN_SCORES
				if (relative_rank(Us, s) == RANK_3)
				{
					score += PawnScoresIsolatedRank3;
				}
#endif
			}

			else if (backward)
				score -= Backward, e->weakUnopposed[Us] += !opposed;

			if (doubled && !supported)
				score -= Doubled;

#ifdef PAWN_SCORES
			bool protected_passed_pawn = false;

			bool passed1 = bool(passed_pawn_mask(Us, s) & ourPawns);

			//File fp1 = file_of(s);
			Rank rp1 = rank_of(s);

			File fp0 = f;
			File fp2 = f;

			if (fp0 > FILE_A)
			{
				fp0 = File(fp0 - 1);
			}

			if (f < FILE_H)
			{
				fp2 = File(fp2 + 1);
			}

			Rank rpp = rp1;

			if (Us == WHITE)
			{
				if (rpp > RANK_2)
				{
					rpp = Rank(rpp - 1);
				}
			}
			else
			{
				if (rpp < RANK_7)
				{
					rpp = Rank(rpp + 1);
				}
				else
				{
					assert(false);
				}
			}

			if (rpp != rp1)
			{
				if (fp0 != f)
				{
					protected_passed_pawn = make_piece(Us, PAWN) == pos.piece_on(make_square(fp0, rpp));
				}

				if (fp2 != f)
				{
					protected_passed_pawn = protected_passed_pawn || (make_piece(Us, PAWN) == pos.piece_on(make_square(fp2, rpp)));
				}

				if (passed1 && protected_passed_pawn)
				{
					score += ProtectedPassedPawn;
				}
			}
#endif
		}

#ifdef PAWN_SCORES
		const Square* pl_1 = pos.squares<PAWN>(Us);

		// Loop through all pawns of the current color and score each pawn
		while ((s = *pl_1++) != SQ_NONE)
		{
			assert(pos.piece_on(s) == make_piece(Us, PAWN));

			File f = file_of(s);

			File f0 = f;
			File f2 = f;

			if (f0 > FILE_A)
			{
				f0 = File(f0 - 1);
			}

			if (f < FILE_H)
			{
				f2 = File(f2 + 1);
			}

			bool passed1 = bool(passed_pawn_mask(Us, s) & ourPawns);

			if (f0 != f)
			{
				bool passed0 = false;

				if (passed1)
				{
					for (Rank r0 = RANK_2; r0 <= RANK_7; r0 = Rank(r0 + 1))
					{
						Square s0 = make_square(f0, r0);

						if (pos.piece_on(s0) == make_piece(Us, PAWN))
						{
							passed0 = e->passedPawns[Us] & s0;

							if (passed0)
							{
								break;
							}
						}
					}

					if (passed0 && passed1)
					{
						score += PawnScoresConnectedPassed;

						Square UsKingSquare = SQ_A1;

						Piece UsKing = make_piece(Us, KING);

						while (pos.piece_on(UsKingSquare) != UsKing)
						{
							UsKingSquare = Square(UsKingSquare + 1);

							assert(UsKingSquare != SQUARE_NB);
						}

						File UsKingFile = file_of(UsKingSquare);
						//Rank UsKingRank = rank_of(UsKingSquare);

						bool connected_passed_defend_king = (UsKingFile >= f0 && UsKingFile <= f2);

						if (connected_passed_defend_king)
						{
							score += KingSafetyCompensationPawnScoresConnectedPassed;
						}
					}
				}
			}
		}
#endif

		return score;
	}
} // namespace

namespace Pawns {

	/// Pawns::init() initializes some tables needed by evaluation. Instead of using
	/// hard-coded tables, when makes sense, we prefer to calculate them with a formula
	/// to reduce independent parameters and to allow easier tuning and better insight.

	void init() {

		static constexpr int Seed[RANK_NB] = { 0, 13, 24, 18, 65, 100, 175, 330 };

		for (int opposed = 0; opposed <= 1; ++opposed)
			for (int phalanx = 0; phalanx <= 1; ++phalanx)
				for (int support = 0; support <= 2; ++support)
					for (Rank r = RANK_2; r < RANK_8; ++r)
					{
						int v = 17 * support;
						v += (Seed[r] + (phalanx ? (Seed[r + 1] - Seed[r]) / 2 : 0)) >> opposed;

						Connected[opposed][phalanx][support][r] = make_score(v, v * (r - 2) / 4);
					}
	}


	/// Pawns::probe() looks up the current position's pawns configuration in
	/// the pawns hash table. It returns a pointer to the Entry if the position
	/// is found. Otherwise a new Entry is computed and stored there, so we don't
	/// have to recompute all when the same pawns configuration occurs again.

	Entry* probe(const Position& pos) {

		Key key = pos.pawn_key();
		Entry* e = pos.this_thread()->pawnsTable[key];

		if (e->key == key)
			return e;

		e->key = key;
		e->scores[WHITE] = evaluate<WHITE>(pos, e);
		e->scores[BLACK] = evaluate<BLACK>(pos, e);
		e->openFiles = popcount(e->semiopenFiles[WHITE] & e->semiopenFiles[BLACK]);
		e->asymmetry = popcount((e->passedPawns[WHITE] | e->passedPawns[BLACK])
			| (e->semiopenFiles[WHITE] ^ e->semiopenFiles[BLACK]));

		return e;
	}


/// Entry::evaluate_shelter() calculates the shelter bonus and the storm
/// penalty for a king, looking at the king file and the two closest files.

template<Color Us>
Value Entry::evaluate_shelter(const Position& pos, Square ksq) {

  enum { UnBlocked, BlockedByPawn };
  constexpr Color     Them = (Us == WHITE ? BLACK : WHITE);
  constexpr Direction Up   = (Us == WHITE ? NORTH : SOUTH);
  constexpr Bitboard  BlockRanks = (Us == WHITE ? Rank2BB | Rank3BB : Rank7BB | Rank6BB);

  Bitboard b = pos.pieces(PAWN) & (forward_ranks_bb(Us, ksq) | rank_bb(ksq));
  Bitboard ourPawns = b & pos.pieces(Us);
  Bitboard theirPawns = b & pos.pieces(Them);

  Value safety = (ourPawns & file_bb(ksq)) ? Value(5) : Value(-5);

  if ((theirPawns & (FileABB | FileHBB) & BlockRanks) & (ksq + Up))
      safety += Value(374);

  File center = std::max(FILE_B, std::min(FILE_G, file_of(ksq)));
  for (File f = File(center - 1); f <= File(center + 1); ++f)
  {
      b = ourPawns & file_bb(f);
      int ourRank = b ? relative_rank(Us, backmost_sq(Us, b)) : 0;

      b = theirPawns & file_bb(f);
      int theirRank = b ? relative_rank(Us, frontmost_sq(Them, b)) : 0;

      int d = std::min(f, ~f);

      safety += ShelterStrength[d][ourRank];
      if (ourRank || theirRank)
         safety -= StormDanger[ourRank && (ourRank == theirRank - 1) ? BlockedByPawn : UnBlocked][d][theirRank];
  }

  return safety;
}


	/// Entry::do_king_safety() calculates a bonus for king safety. It is called only
	/// when king square changes, which is about 20% of total king_safety() calls.

	template<Color Us>
	Score Entry::do_king_safety(const Position& pos, Square ksq) {

		kingSquares[Us] = ksq;
		castlingRights[Us] = pos.can_castle(Us);
		int minKingPawnDistance = 0;

		Bitboard pawns = pos.pieces(Us, PAWN);
		if (pawns)
			while (!(DistanceRingBB[ksq][minKingPawnDistance++] & pawns)) {}


  Value bonus = evaluate_shelter<Us>(pos, ksq);

  // If we can castle use the bonus after the castling if it is bigger
  if (pos.can_castle(MakeCastling<Us, KING_SIDE>::right))
      bonus = std::max(bonus, evaluate_shelter<Us>(pos, relative_square(Us, SQ_G1)));

  if (pos.can_castle(MakeCastling<Us, QUEEN_SIDE>::right))
      bonus = std::max(bonus, evaluate_shelter<Us>(pos, relative_square(Us, SQ_C1)));

		return make_score(bonus, -16 * minKingPawnDistance);
	}

	// Explicit template instantiation
	template Score Entry::do_king_safety<WHITE>(const Position& pos, Square ksq);
	template Score Entry::do_king_safety<BLACK>(const Position& pos, Square ksq);

} // namespace Pawns
