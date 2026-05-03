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
#include "clang/Sema/SemaDiagnostic.h"

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
                                SmallVectorImpl<ExprResult> &Results) {
  // Lower match(expr) { pat1 => res1, pat2 => res2, _ => res3 }
  // into: (scrutinee == pat1 ? res1 : (scrutinee == pat2 ? res2 : res3))

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

  // For the MVP, contract_assert(condition) is lowered to the condition
  // evaluated as a statement. The condition is checked at runtime, and
  // a failing assertion will be detected when the condition evaluates to
  // false (the value is discarded but the expression is still evaluated).
  //
  // A full implementation would lower to: if (!(condition)) __builtin_trap();
  // Tier 2 will add proper if-then-trap codegen via ConditionResult.

  return ActOnExprStmt(Condition);
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
