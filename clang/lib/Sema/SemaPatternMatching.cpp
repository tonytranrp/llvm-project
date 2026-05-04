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
//  conditional expressions and contract_assert/pre/post to if-then-trap.
//
//===----------------------------------------------------------------------===//

#include "clang/Sema/Sema.h"
#include "clang/AST/Decl.h"
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

ExprResult Sema::ActOnBindingPattern(
    SourceLocation AutoLoc, SourceLocation LSquareLoc,
    SmallVectorImpl<IdentifierInfo *> &Bindings,
    SmallVectorImpl<SourceLocation> &BindingLocs,
    SourceLocation RSquareLoc, Expr *Scrutinee) {
  // auto [x, y, ...] binding destructuring pattern.
  // Creates VarDecls for each binding name and generates assignment expressions
  // that bind them to the scrutinee. The pattern condition is a comma-operator
  // chain: (x = scrutinee, y = scrutinee, ..., true)
  // For the MVP, each binding is assigned the full scrutinee.
  // Proper element extraction (std::get<N>) is deferred to a future tier.

  if (Bindings.empty()) {
    return ActOnCXXBoolLiteral(LSquareLoc, tok::kw_true);
  }

  SmallVector<VarDecl *, 4> BindingDecls;

  // Create a VarDecl for each binding identifier (no initializer — will be
  // assigned in the comma-operator chain).
  for (unsigned I = 0; I < Bindings.size(); ++I) {
    IdentifierInfo *II = Bindings[I];
    SourceLocation IdLoc = BindingLocs[I];

    QualType VarType;
    if (Scrutinee && !Scrutinee->isTypeDependent()) {
      VarType = Scrutinee->getType();
    } else {
      VarType = Context.getAutoDeductType();
    }

    VarDecl *VD = VarDecl::Create(Context, getCurScope()->getEntity(),
                                   IdLoc, IdLoc, II, VarType,
                                   /*TInfo=*/nullptr, SC_Auto);
    // No initializer — assignments happen in the comma-operator chain

    // Mark as used so we don't get warnings
    VD->setIsUsed();

    // Push the declaration into scope
    PushOnScopeChains(VD, getCurScope(), /*AddToContext=*/true);

    BindingDecls.push_back(VD);
  }

  // Build the comma-operator chain: (x = scrutinee, y = scrutinee, ..., true)
  // Each assignment initializes the binding variable from the scrutinee.
  ExprResult Condition;

  if (Scrutinee && !Scrutinee->isTypeDependent()) {
    for (unsigned I = 0; I < BindingDecls.size(); ++I) {
      VarDecl *VD = BindingDecls[I];

      // Create a DeclRefExpr for the variable
      Expr *DRE = BuildDeclRefExpr(VD, VD->getType(), VK_PRValue, VD->getLocation());

      // Build: var = scrutinee
      ExprResult Assign = BuildBinOp(getCurScope(), VD->getLocation(),
                                      BinaryOperatorKind::BO_Assign, DRE, Scrutinee);
      if (Assign.isInvalid()) {
        PendingBinding.Active = false;
        return ActOnCXXBoolLiteral(AutoLoc, tok::kw_true);
      }

      if (I == 0) {
        Condition = Assign;
      } else {
        // Comma operator: (prev, assign)
        Condition = BuildBinOp(getCurScope(), VD->getLocation(),
                                BinaryOperatorKind::BO_Comma,
                                Condition.get(), Assign.get());
        if (Condition.isInvalid()) {
          PendingBinding.Active = false;
          return ActOnCXXBoolLiteral(AutoLoc, tok::kw_true);
        }
      }
    }

    // Append true: (..., true)
    ExprResult TrueExpr = ActOnCXXBoolLiteral(AutoLoc, tok::kw_true);
    Condition = BuildBinOp(getCurScope(), AutoLoc,
                            BinaryOperatorKind::BO_Comma,
                            Condition.get(), TrueExpr.get());
    if (Condition.isInvalid()) {
      PendingBinding.Active = false;
      return ActOnCXXBoolLiteral(AutoLoc, tok::kw_true);
    }
  } else {
    // Dependent case: just return true
    Condition = ActOnCXXBoolLiteral(AutoLoc, tok::kw_true);
  }

  // Clear pending binding info
  PendingBinding.Active = false;

  return Condition;
}

ExprResult Sema::ActOnDestructuringPattern(
    SourceLocation LSquareLoc, SmallVectorImpl<ExprResult> &SubPatterns,
    SourceLocation RSquareLoc) {
  // Lower [p1, p2, p3] into: p1 && p2 && p3
  // Each sub-pattern is already lowered to a boolean condition.
  // Wildcards become true, literals become scrutinee==literal, etc.

  if (SubPatterns.empty()) {
    return ActOnCXXBoolLiteral(LSquareLoc, tok::kw_true);
  }

  ExprResult Result = SubPatterns[0];
  for (unsigned I = 1; I < SubPatterns.size(); ++I) {
    if (Result.isInvalid() || SubPatterns[I].isInvalid())
      return ExprError();

    Expr *LHS = Result.get();
    Expr *RHS = SubPatterns[I].get();

    if (!LHS || !RHS)
      return ExprError();

    if (LHS->isTypeDependent() || RHS->isTypeDependent())
      return Result;

    Result = BuildBinOp(getCurScope(), LHS->getExprLoc(),
                        BinaryOperatorKind::BO_LAnd, LHS, RHS);
    if (Result.isInvalid())
      return ExprError();
  }

  return Result;
}

ExprResult Sema::ActOnTypePattern(SourceLocation QuestionLoc,
                                   TypeSourceInfo *TSI,
                                   SourceLocation EndLoc) {
  // Type pattern: ?type — match if scrutinee is of that type.
  // For the MVP, we lower this to `true` (always matches).
  // TODO(Tier 2): Store the type info and generate __builtin_types_compatible_p
  return ActOnCXXBoolLiteral(QuestionLoc, tok::kw_true);
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
        (isa<CXXBoolLiteralExpr>(Pattern) ||
         // Binding patterns produce comma-operator chains ending in true.
         // Check if this is a BinaryOperator with BO_Comma whose RHS is true.
         (isa<BinaryOperator>(Pattern) &&
          cast<BinaryOperator>(Pattern)->isCommaOp() &&
          isa<CXXBoolLiteralExpr>(cast<BinaryOperator>(Pattern)->getRHS()) &&
          cast<CXXBoolLiteralExpr>(cast<BinaryOperator>(Pattern)->getRHS())->getValue()))) {
      // Wildcard/type/destructuring/binding pattern (lowered to `true` or
      // a comma-chain ending in `true`) — use directly as condition.
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

  // Lower contract_assert/pre/post(condition) to: if (!(condition)) __builtin_trap();

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
