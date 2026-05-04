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

/// ParsePattern - Parse a single pattern inside a match expression.
/// Supported patterns:
///   - _ (wildcard)
///   - ?type (type pattern — match if scrutinee is of that type)
///   - auto [x, y, ...] (binding destructuring pattern)
///   - expression (literal pattern, matched with ==)
///   - [pattern, pattern, ...] (destructuring pattern)
ExprResult Parser::ParsePattern() {
  // Wildcard pattern: _
  if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()->isStr("_")) {
    SourceLocation UnderscoreLoc = ConsumeToken();
    return Actions.ActOnWildcardPattern(UnderscoreLoc);
  }

  // Type pattern: ?type — match if scrutinee is of that type
  if (Tok.is(tok::question) && getLangOpts().PatternMatching) {
    return ParseTypePattern();
  }

  // Binding destructuring pattern: auto [x, y, ...]
  if (Tok.is(tok::kw_auto) && getLangOpts().PatternMatching) {
    // Look ahead to see if next token is [
    Token Next = NextToken();
    if (Next.is(tok::l_square)) {
      return ParseBindingPattern();
    }
    // Otherwise fall through to expression parsing (auto as type in expr)
  }

  // Destructuring pattern: [pattern, pattern, ...]
  if (Tok.is(tok::l_square)) {
    return ParseDestructuringPattern();
  }

  // Expression pattern (fallback)
  return ParseConditionalExpression();
}

/// ParseDestructuringPattern - Parse a destructuring pattern: [p1, p2, ...]
/// Lowered to: p1 && p2 && ... (each sub-pattern is already a boolean)
ExprResult Parser::ParseDestructuringPattern() {
  assert(Tok.is(tok::l_square) && "Expected [ for destructuring pattern");
  SourceLocation LSquareLoc = ConsumeToken(); // consume [

  SmallVector<ExprResult, 4> SubPatterns;

  // Parse comma-separated sub-patterns
  while (Tok.isNot(tok::r_square) && Tok.isNot(tok::eof)) {
    ExprResult SubPattern = ParsePattern();
    if (SubPattern.isInvalid()) {
      SkipUntil(tok::r_square, StopBeforeMatch);
      return ExprError();
    }
    SubPatterns.push_back(SubPattern);

    if (Tok.is(tok::comma))
      ConsumeToken(); // consume ,
    else
      break;
  }

  if (!Tok.is(tok::r_square)) {
    Diag(Tok, diag::err_expected) << tok::r_square;
    return ExprError();
  }
  SourceLocation RSquareLoc = ConsumeToken(); // consume ]

  return Actions.ActOnDestructuringPattern(LSquareLoc, SubPatterns,
                                            RSquareLoc);
}

/// ParseTypePattern - Parse a type pattern: ?type
/// match(x) { ?int => 1, ?double => 2, _ => 0 }
ExprResult Parser::ParseTypePattern() {
  assert(Tok.is(tok::question) && "Expected ? for type pattern");
  SourceLocation QuestionLoc = ConsumeToken(); // consume ?

  // Parse the type after ?
  TypeResult TR = ParseTypeName();
  if (TR.isInvalid())
    return ExprError();

  TypeSourceInfo *TSI = nullptr;
  QualType QT = Actions.GetTypeFromParser(TR.get(), &TSI);
  if (QT.isNull())
    return ExprError();
  if (!TSI)
    TSI = Actions.getASTContext().getTrivialTypeSourceInfo(QT, QuestionLoc);

  return Actions.ActOnTypePattern(QuestionLoc, TSI, QuestionLoc,
                                  MatchScrutinee);
}

/// ParseBindingPattern - Parse a binding destructuring pattern: auto [x, y, ...]
/// Binds identifiers to tuple/struct elements of the scrutinee.
ExprResult Parser::ParseBindingPattern() {
  assert(Tok.is(tok::kw_auto) && "Expected 'auto' for binding pattern");
  SourceLocation AutoLoc = ConsumeToken(); // consume 'auto'

  if (!Tok.is(tok::l_square)) {
    Diag(Tok, diag::err_expected) << tok::l_square;
    return ExprError();
  }
  SourceLocation LSquareLoc = ConsumeToken(); // consume [

  SmallVector<IdentifierInfo *, 4> Bindings;
  SmallVector<SourceLocation, 4> BindingLocs;

  // Parse comma-separated identifiers
  while (Tok.isNot(tok::r_square) && Tok.isNot(tok::eof)) {
    if (!Tok.is(tok::identifier)) {
      Diag(Tok, diag::err_expected) << "identifier";
      SkipUntil(tok::r_square, StopBeforeMatch);
      return ExprError();
    }
    IdentifierInfo *II = Tok.getIdentifierInfo();
    SourceLocation IdLoc = ConsumeToken();
    Bindings.push_back(II);
    BindingLocs.push_back(IdLoc);

    if (Tok.is(tok::comma))
      ConsumeToken(); // consume ,
    else
      break;
  }

  if (!Tok.is(tok::r_square)) {
    Diag(Tok, diag::err_expected) << tok::r_square;
    return ExprError();
  }
  SourceLocation RSquareLoc = ConsumeToken(); // consume ]

  return Actions.ActOnBindingPattern(AutoLoc, LSquareLoc, Bindings,
                                      BindingLocs, RSquareLoc,
                                      MatchScrutinee);
}

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

  // Store scrutinee for binding pattern access
  MatchScrutinee = Scrutinee.get();

  BalancedDelimiterTracker BraceT(*this, tok::l_brace);
  if (BraceT.expectAndConsume())
    return ExprError();

  SourceLocation LBraceLoc = BraceT.getOpenLocation();

  SmallVector<ExprResult, 8> Patterns;
  SmallVector<ExprResult, 8> Results;
  SmallVector<SourceLocation, 8> ArrowLocs;
  SmallVector<ExprResult, 8> Guards;

  // Parse pattern => expr pairs
  while (Tok.isNot(tok::r_brace) && Tok.isNot(tok::eof)) {
    ExprResult Pattern = ParsePattern();
    if (Pattern.isInvalid()) {
      SkipUntil(tok::r_brace, StopBeforeMatch);
      break;
    }

    // Parse optional guard: if condition
    ExprResult Guard;
    if (Tok.is(tok::kw_if)) {
      ConsumeToken(); // consume 'if'
      Guard = ParseAssignmentExpression();
      if (Guard.isInvalid()) {
        SkipUntil(tok::r_brace, StopBeforeMatch);
        break;
      }
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
    Guards.push_back(Guard);

    // Optional comma separator
    if (Tok.is(tok::comma))
      ConsumeToken();
  }

  BraceT.consumeClose();
  SourceLocation RBraceLoc = BraceT.getCloseLocation();

  // Clear scrutinee pointer
  MatchScrutinee = nullptr;

  return Actions.ActOnMatchExpr(MatchLoc, RParenLoc, LBraceLoc, RBraceLoc,
                                Scrutinee.get(), Patterns, ArrowLocs, Results,
                                Guards);
}
