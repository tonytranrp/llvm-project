//===--- ParsePatternMatching.cpp - C++ Pattern Matching Parsing ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Parse/Parser.h"
#include "clang/Parse/RAIIObjectsForParser.h"
#include "clang/Sema/Sema.h"

using namespace clang;

/// ParseMatchExpression - Parse a match expression:
///   match(expr) { pattern => expr, ... }
ExprResult Parser::ParseMatchExpression() {
  assert(Tok.is(tok::kw_match) && "Expected 'match' keyword");
  SourceLocation MatchLoc = ConsumeToken(); // consume 'match'

  BalancedDelimiterTracker T(*this, tok::l_paren);
  if (T.expectAndConsume())
    return ExprError();

  ExprResult Scrutinee = ParseExpression();
  if (Scrutinee.isInvalid()) {
    T.skipToEnd();
    return ExprError();
  }

  T.consumeClose();
  SourceLocation RParenLoc = T.getCloseLocation();

  BalancedDelimiterTracker BraceT(*this, tok::l_brace);
  if (BraceT.expectAndConsume())
    return ExprError();

  SourceLocation LBraceLoc = BraceT.getOpenLocation();

  SmallVector<ExprResult, 8> Patterns;
  SmallVector<ExprResult, 8> Results;
  SmallVector<SourceLocation, 8> ArrowLocs;

  // Parse pattern => expr pairs
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    // Parse a pattern:
    //  - _ (wildcard)
    //  - identifier (binding)
    //  - expression followed by =>
    //  - expression if guard followed by =>
    ExprResult Pattern;

    // Special case: wildcard _
    if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()->isStr("_")) {
      SourceLocation UnderscoreLoc = ConsumeToken();
      Pattern = Actions.ActOnWildcardPattern(UnderscoreLoc);
    } else {
      // Parse expression that will be matched against the scrutinee.
      // This handles literal patterns, type patterns, etc.
      // We use ParseConstantExpression to avoid consuming => as part of
      // the expression.
      Pattern = ParseConditionalExpression();
    }

    if (Pattern.isInvalid()) {
      SkipUntil(tok::r_brace, StopBeforeMatch);
      break;
    }

    // Parse optional guard: if condition
    // TODO: Guard expressions are parsed but not yet used in the MVP lowering.
    // They will be combined with the pattern condition: (scrutinee == pattern) && guard
    if (Tok.is(tok::kw_if)) {
      ConsumeToken(); // consume 'if'
      ExprResult Guard = ParseAssignmentExpression();
      if (Guard.isInvalid()) {
        SkipUntil(tok::r_brace, StopBeforeMatch);
        break;
      }
      // TODO: Attach guard to the pattern for Sema
      (void)Guard;
    }

    // Expect => (equalgreater token)
    if (!Tok.is(tok::equalgreater)) {
      Diag(Tok.getLocation(), diag::err_expected) << "=>";
      SkipUntil(tok::r_brace, StopBeforeMatch);
      break;
    }
    SourceLocation ArrowLoc = ConsumeToken();

    // Parse result expression
    ExprResult Result = ParseAssignmentExpression();
    if (Result.isInvalid()) {
      SkipUntil(tok::r_brace, StopBeforeMatch);
      break;
    }

    Patterns.push_back(Pattern);
    Results.push_back(Result);
    ArrowLocs.push_back(ArrowLoc);

    // Optional comma separator
    if (Tok.is(tok::comma))
      ConsumeToken();
  }

  BraceT.consumeClose();
  SourceLocation RBraceLoc = BraceT.getCloseLocation();

  return Actions.ActOnMatchExpr(MatchLoc, RParenLoc, LBraceLoc, RBraceLoc,
                                Scrutinee.get(), Patterns, ArrowLocs, Results);
}
