//===--- SemaPatternMatching.cpp - Pattern Matching & Contracts Sema -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for C++ pattern matching and
//  contracts extensions. For the MVP, match expressions are lowered to
//  conditional expressions and contract_assert to if-then-trap.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/Sema.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/DeclSpec.h"

using namespace clang;

ExprResult Sema::ActOnWildcardPattern(SourceLocation UnderscoreLoc) {
  return ActOnCXXBoolLiteral(UnderscoreLoc, tok::kw_true);
}

ExprResult Sema::ActOnIdentifierPattern(SourceLocation IdLoc,
                                        IdentifierInfo *II) {
  // TODO(Tier 2): Create a proper binding pattern that introduces a variable.
  return ActOnCXXBoolLiteral(IdLoc, tok::kw_true);
}

ExprResult Sema::ActOnMatchExpr(SourceLocation MatchLoc,
                                SourceLocation RParenLoc,
                                SourceLocation LBraceLoc,
                                SourceLocation RBraceLoc,
                                Expr *Scrutinee,
                                SmallVectorImpl<ExprResult> &Patterns,
                                SmallVectorImpl<SourceLocation> &ArrowLocs,
                                SmallVectorImpl<ExprResult> &Results,
                                SmallVectorImpl<ExprResult> &Guards) {
  // Lower match(expr) { pat1 => res1, pat2 => res2, _ => res3 }
  // into: (scrutinee == pat1 ? res1 : (scrutinee == pat2 ? res2 : res3))
  // With guards: match(expr) { pat1 if guard1 => res1, ... }
  // becomes: ((scrutinee == pat1) && guard1 ? res1 : ...)

  if (Results.empty())
    return ExprError();

  if (!Scrutinee)
    return ExprError();

  ExprResult Result = Results.back().get();
  if (Result.isInvalid())
    return ExprError();

  for (int I = (int)Patterns.size() - 1; I >= 0; --I) {
    Expr *Pattern = Patterns[I].get();
    Expr *ThenExpr = Results[I].get();

    if (!Pattern || !ThenExpr || Pattern->isTypeDependent() ||
        ThenExpr->isTypeDependent() || Scrutinee->isTypeDependent()) {
      return Result;
    }

    ExprResult Condition;
    if (Pattern->getType()->isBooleanType() &&
        isa<CXXBoolLiteralExpr>(Pattern)) {
      // Wildcard pattern (lowered to `true`) — use directly.
      Condition = Pattern;
    } else {
      // Build: scrutinee == pattern
      Condition = BuildBinOp(getCurScope(), Scrutinee->getExprLoc(),
                             BinaryOperatorKind::BO_EQ, Scrutinee, Pattern);
      if (Condition.isInvalid())
        return ExprError();
    }

    // If there's a guard, combine with pattern condition: condition && guard
    if (I < (int)Guards.size() && Guards[I].isUsable()) {
      Expr *Guard = Guards[I].get();
      if (Guard && !Guard->isTypeDependent()) {
        ExprResult GuardBool = PerformContextuallyConvertToBool(Guard);
        if (GuardBool.isUsable()) {
          Condition = BuildBinOp(getCurScope(), Guard->getExprLoc(),
                                 BinaryOperatorKind::BO_LAnd,
                                 Condition.get(), GuardBool.get());
          if (Condition.isInvalid())
            return ExprError();
        }
      }
    }

    Result = ActOnConditionalOp(Condition.get()->getExprLoc(),
                                ArrowLocs[I],
                                Condition.get(), ThenExpr, Result.get());
    if (Result.isInvalid())
      return ExprError();
  }

  return Result;
}

//===----------------------------------------------------------------------===//
// Contracts Sema Actions
//===----------------------------------------------------------------------===//

StmtResult Sema::ActOnContractAssertStmt(SourceLocation Loc, Expr *Condition,
                                          StringLiteral *Message) {
  if (!getLangOpts().Contracts) {
    Diag(Loc, diag::err_contracts_required);
    return StmtError();
  }

  if (!Condition)
    return StmtError();

  if (!Condition->isTypeDependent()) {
    ExprResult CondRes = PerformContextuallyConvertToBool(Condition);
    if (!CondRes.isUsable())
      return StmtError();
    Condition = CondRes.get();
  }

  // Lower contract_assert(condition) to: if (!(condition)) __builtin_trap();
  // This provides runtime contract checking — if the condition is false,
  // the program traps immediately (SIGILL on most platforms).

  // Step 1: Build !(condition)
  ExprResult NegatedCond = ActOnUnaryOp(getCurScope(), Loc, tok::exclaim, Condition);
  if (NegatedCond.isInvalid())
    return StmtError();

  // Step 2: Build __builtin_trap() call
  CXXScopeSpec SS;
  SourceLocation TemplateKWLoc;
  UnqualifiedId TrapName;
  TrapName.setIdentifier(PP.getIdentifierInfo("__builtin_trap"), Loc);
  ExprResult TrapFn = ActOnIdExpression(
      getCurScope(), SS, TemplateKWLoc, TrapName,
      /*HasTrailingLParen=*/true, /*IsAddressOfOperand=*/false);
  if (TrapFn.isInvalid())
    return StmtError();

  ExprResult TrapCall = BuildCallExpr(getCurScope(), TrapFn.get(), Loc, {}, Loc);
  if (TrapCall.isInvalid())
    return StmtError();

  // Step 3: Wrap the trap call in a statement
  Stmt *ThenStmt = TrapCall.getAs<Expr>();
  if (!ThenStmt)
    return StmtError();

  // Step 4: Create the if-statement: if (!(cond)) __builtin_trap();
  IfStmt *If = IfStmt::Create(Context, Loc, IfStatementKind::Ordinary,
                               /*Init=*/nullptr, /*Var=*/nullptr,
                               NegatedCond.getAs<Expr>(),
                               /*LParenLoc=*/Loc, /*RParenLoc=*/Loc,
                               ThenStmt,
                               /*ElseLoc=*/SourceLocation(),
                               /*Else=*/nullptr);
  return If;
}

void Sema::ActOnFunctionContractPre(SourceLocation Loc, Expr *Condition,
                                     StringLiteral *Message) {
  if (!getLangOpts().Contracts)
    Diag(Loc, diag::err_contracts_required);
}

void Sema::ActOnFunctionContractPost(SourceLocation Loc, Expr *Condition,
                                      StringLiteral *Message) {
  if (!getLangOpts().Contracts)
    Diag(Loc, diag::err_contracts_required);
}
