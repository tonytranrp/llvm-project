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
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Basic/TypeTraits.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/SemaDiagnostic.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/DeclSpec.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang;

ExprResult Sema::ActOnWildcardPattern(SourceLocation UnderscoreLoc) {
  // Create !false as the lowered form of wildcard _.
  // This evaluates to true (same CodeGen), but can be distinguished from
  // a literal `true` pattern by classifyPattern for exhaustiveness checking.
  ExprResult FalseLit = ActOnCXXBoolLiteral(UnderscoreLoc, tok::kw_false);
  return CreateBuiltinUnaryOp(UnderscoreLoc, UnaryOperatorKind::UO_LNot,
                              FalseLit.get());
}

ExprResult Sema::ActOnIdentifierPattern(SourceLocation IdLoc,
                                        IdentifierInfo *II) {
  // TODO(Tier 2): Create a proper binding pattern that introduces a variable.
  // For now, use !false (same as wildcard _) since identifier bindings
  // match everything and act as wildcards for coverage purposes.
  ExprResult FalseLit = ActOnCXXBoolLiteral(IdLoc, tok::kw_false);
  return CreateBuiltinUnaryOp(IdLoc, UnaryOperatorKind::UO_LNot,
                              FalseLit.get());
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
                                   SourceLocation EndLoc,
                                   Expr *Scrutinee) {
  // Type pattern: ?type — match if scrutinee is of that type.
  // We generate __is_same(decltype(scrutinee), type) at compile time.
  // This uses the TypeTraitExpr mechanism which folds to a boolean constant
  // when both types are non-dependent.

  if (!TSI || !Scrutinee) {
    return ActOnCXXBoolLiteral(QuestionLoc, tok::kw_true);
  }

  QualType PatternType = TSI->getType();
  QualType ScrutineeType = Scrutinee->getType();

  if (ScrutineeType.isNull() || PatternType.isNull() ||
      ScrutineeType->isDependentType() || PatternType->isDependentType()) {
    // Dependent types — can't evaluate yet. Lower to true as fallback.
    return ActOnCXXBoolLiteral(QuestionLoc, tok::kw_true);
  }

  // Build a TypeTraitExpr for __is_same(decltype(scrutinee), pattern_type)
  // which evaluates at compile time.
  TypeSourceInfo *ScrutineeTSI =
      Context.getTrivialTypeSourceInfo(ScrutineeType, QuestionLoc);

  SmallVector<TypeSourceInfo *, 2> TraitArgs;
  TraitArgs.push_back(ScrutineeTSI);
  TraitArgs.push_back(TSI);

  bool IsSame = Context.hasSameType(ScrutineeType, PatternType);
  return TypeTraitExpr::Create(Context, Context.getLogicalOperationType(),
                               QuestionLoc, BTT_IsSame, TraitArgs, EndLoc,
                               IsSame);
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

  // Run exhaustiveness checking before lowering to ternary conditionals.
  CheckMatchExhaustiveness(MatchLoc, Scrutinee, Patterns, ArrowLocs,
                           Results, Guards);

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
         // TypeTraitExpr from type patterns (?type) — use directly as condition.
         isa<TypeTraitExpr>(Pattern) ||
         // Binding patterns produce comma-operator chains ending in true.
         // Check if this is a BinaryOperator with BO_Comma whose RHS is true.
         (isa<BinaryOperator>(Pattern) &&
          cast<BinaryOperator>(Pattern)->isCommaOp() &&
          isa<CXXBoolLiteralExpr>(cast<BinaryOperator>(Pattern)->getRHS()) &&
          cast<CXXBoolLiteralExpr>(cast<BinaryOperator>(Pattern)->getRHS())->getValue()))) {
      // Wildcard/type/destructuring/binding pattern (lowered to `true` or
      // a comma-chain ending in `true`) or type pattern (TypeTraitExpr) —
      // use directly as condition.
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
// Pattern Matching Exhaustiveness Checking
//===----------------------------------------------------------------------===//

Sema::PatternKind Sema::classifyPattern(Expr *Pattern) {
  if (!Pattern)
    return PatternKind::Unknown;

  // Wildcard pattern: lowered to UnaryOperator(!false) by ActOnWildcardPattern.
  // Detect !false as the wildcard marker.
  if (auto *UnaryOp = dyn_cast<UnaryOperator>(Pattern)) {
    if (UnaryOp->getOpcode() == UO_LNot) {
      if (auto *InnerBool = dyn_cast<CXXBoolLiteralExpr>(UnaryOp->getSubExpr())) {
        if (!InnerBool->getValue()) // !false = wildcard
          return PatternKind::Wildcard;
      }
    }
  }

  // CXXBoolLiteralExpr: true/false are literals (not wildcards).
  if (auto *BoolLit = dyn_cast<CXXBoolLiteralExpr>(Pattern)) {
    return PatternKind::Literal;
  }

  // Identifier binding pattern: lowered to UnaryOperator(!false) by
  // ActOnIdentifierPattern. Acts as wildcard for coverage purposes.

  // Binding destructuring pattern: auto [x, y] lowered to comma-operator chain
  // whose RHS is a wildcard pattern (UnaryOperator(!false)). Check for
  // BinaryOperator with BO_Comma whose RHS is a wildcard marker.
  if (auto *BinOp = dyn_cast<BinaryOperator>(Pattern)) {
    if (BinOp->isCommaOp()) {
      // Check if the RHS is a wildcard marker (!false)
      if (classifyPattern(BinOp->getRHS()) == PatternKind::Wildcard)
        return PatternKind::Wildcard; // binding pattern acts as wildcard
    }
    // Destructuring pattern [p1, p2] lowered to p1 && p2
    if (BinOp->getOpcode() == BO_LAnd) {
      return PatternKind::Destructuring;
    }
  }

  // Type pattern: ?type lowered to TypeTraitExpr
  if (isa<TypeTraitExpr>(Pattern)) {
    return PatternKind::TypePattern;
  }

  // Integer literal, enum constant, or other expression pattern
  if (isa<IntegerLiteral>(Pattern) || isa<EnumConstantDecl>(
      Pattern->getReferencedDeclOfCallee())) {
    return PatternKind::Literal;
  }

  // DeclRefExpr referencing an enum constant
  if (auto *DRE = dyn_cast<DeclRefExpr>(Pattern)) {
    if (isa<EnumConstantDecl>(DRE->getDecl()))
      return PatternKind::Literal;
  }

  // MemberExpr referencing an enum constant
  if (auto *ME = dyn_cast<MemberExpr>(Pattern)) {
    if (isa<EnumConstantDecl>(ME->getMemberDecl()))
      return PatternKind::Literal;
  }

  // Any other expression that can be evaluated to a constant integer value
  // (covers cases like `1 + 0`, static_cast<MyEnum>(0), etc.)
  if (Pattern->isEvaluatable(Context)) {
    return PatternKind::Literal;
  }

  // Character literal
  if (isa<CharacterLiteral>(Pattern))
    return PatternKind::Literal;

  // Floating literal
  if (isa<FloatingLiteral>(Pattern))
    return PatternKind::Literal;

  // String literal
  if (isa<StringLiteral>(Pattern))
    return PatternKind::Literal;

  return PatternKind::Unknown;
}

bool Sema::patternSubsumes(Expr *PatternA, Expr *PatternB) {
  if (!PatternA || !PatternB)
    return false;

  PatternKind KindA = classifyPattern(PatternA);
  PatternKind KindB = classifyPattern(PatternB);

  // Wildcard subsumes everything
  if (KindA == PatternKind::Wildcard)
    return true;

  // TypePattern subsumes another TypePattern with the same type
  if (KindA == PatternKind::TypePattern && KindB == PatternKind::TypePattern) {
    auto *TraitA = cast<TypeTraitExpr>(PatternA);
    auto *TraitB = cast<TypeTraitExpr>(PatternB);
    // If both are __is_same with the same second type argument, A subsumes B
    if (TraitA->getNumArgs() == 2 && TraitB->getNumArgs() == 2) {
      if (Context.hasSameType(TraitA->getArg(1)->getType(),
                              TraitB->getArg(1)->getType()))
        return true;
    }
    return false;
  }

  // Literal subsumes the same literal value
  if (KindA == PatternKind::Literal && KindB == PatternKind::Literal) {
    // For enum constants, check if they refer to the same declaration
    EnumConstantDecl *EnumA = nullptr, *EnumB = nullptr;
    if (auto *DRE = dyn_cast<DeclRefExpr>(PatternA))
      EnumA = dyn_cast<EnumConstantDecl>(DRE->getDecl());
    if (auto *ME = dyn_cast<MemberExpr>(PatternA))
      EnumA = dyn_cast<EnumConstantDecl>(ME->getMemberDecl());
    if (auto *DRE = dyn_cast<DeclRefExpr>(PatternB))
      EnumB = dyn_cast<EnumConstantDecl>(DRE->getDecl());
    if (auto *ME = dyn_cast<MemberExpr>(PatternB))
      EnumB = dyn_cast<EnumConstantDecl>(ME->getMemberDecl());

    if (EnumA && EnumB)
      return EnumA == EnumB;

    // For integer literals, check constant-evaluated equality
    if (PatternA->isIntegerConstantExpr(Context) &&
        PatternB->isIntegerConstantExpr(Context)) {
      llvm::APSInt ValueA = PatternA->EvaluateKnownConstInt(Context);
      llvm::APSInt ValueB = PatternB->EvaluateKnownConstInt(Context);
      return ValueA == ValueB;
    }

    // For boolean literals
    if (auto *BoolA = dyn_cast<CXXBoolLiteralExpr>(PatternA)) {
      if (auto *BoolB = dyn_cast<CXXBoolLiteralExpr>(PatternB)) {
        return BoolA->getValue() == BoolB->getValue();
      }
    }

    return false;
  }

  // Destructuring pattern subsumes another destructuring pattern if each
  // sub-pattern subsumes
  if (KindA == PatternKind::Destructuring &&
      KindB == PatternKind::Destructuring) {
    // Collect sub-patterns from the && chains
    SmallVector<Expr *, 4> SubPatsA, SubPatsB;
    auto collectAndOperands = [](Expr *E, SmallVector<Expr *, 4> &Out) {
      SmallVector<Expr *, 8> Worklist;
      Worklist.push_back(E);
      while (!Worklist.empty()) {
        Expr *Cur = Worklist.pop_back_val();
        if (auto *BO = dyn_cast<BinaryOperator>(Cur)) {
          if (BO->getOpcode() == BO_LAnd) {
            Worklist.push_back(BO->getLHS());
            Worklist.push_back(BO->getRHS());
            continue;
          }
        }
        Out.push_back(Cur);
      }
    };
    collectAndOperands(PatternA, SubPatsA);
    collectAndOperands(PatternB, SubPatsB);
    if (SubPatsA.size() != SubPatsB.size())
      return false;
    for (unsigned I = 0; I < SubPatsA.size(); ++I) {
      if (!patternSubsumes(SubPatsA[I], SubPatsB[I]))
        return false;
    }
    return true;
  }

  return false;
}

void Sema::CheckMatchExhaustiveness(
    SourceLocation MatchLoc, Expr *Scrutinee,
    SmallVectorImpl<ExprResult> &Patterns,
    SmallVectorImpl<SourceLocation> &ArrowLocs,
    SmallVectorImpl<ExprResult> &Results,
    SmallVectorImpl<ExprResult> &Guards) {

  if (!Scrutinee || Scrutinee->isTypeDependent())
    return;

  if (Patterns.empty())
    return;

  QualType ScrutineeType = Scrutinee->getType();
  if (ScrutineeType.isNull())
    return;

  // Check for unreachable patterns first
  for (unsigned I = 0; I < Patterns.size(); ++I) {
    Expr *PatI = Patterns[I].get();
    if (!PatI || PatI->isTypeDependent())
      continue;

    // A pattern is unreachable if any earlier pattern subsumes it.
    // But: if the earlier pattern has a guard, it does NOT fully subsume
    // a later pattern (the guard may fail).
    for (unsigned J = 0; J < I; ++J) {
      Expr *PatJ = Patterns[J].get();
      if (!PatJ || PatJ->isTypeDependent())
        continue;

      // If pattern J has a guard, it does not fully subsume pattern I
      // because the guard might fail.
      bool JHasGuard = (J < Guards.size() && Guards[J].isUsable() &&
                        Guards[J].get() != nullptr);
      if (JHasGuard)
        continue;

      if (patternSubsumes(PatJ, PatI)) {
        Diag(PatI->getExprLoc(), diag::warn_unreachable_pattern);
        break; // Only warn once per pattern
      }
    }
  }

  // Check exhaustiveness: determine if all possible values of the scrutinee
  // type are covered by some pattern.

  // First, check if any wildcard pattern exists — if so, the match is
  // exhaustive (for the simple cases we handle). But wildcard with a guard
  // may not cover all cases.
  bool HasWildcardWithoutGuard = false;
  for (unsigned I = 0; I < Patterns.size(); ++I) {
    Expr *Pat = Patterns[I].get();
    if (!Pat)
      continue;
    PatternKind Kind = classifyPattern(Pat);
    bool HasGuard = (I < Guards.size() && Guards[I].isUsable() &&
                     Guards[I].get() != nullptr);
    if ((Kind == PatternKind::Wildcard) && !HasGuard) {
      HasWildcardWithoutGuard = true;
      break;
    }
  }

  // For enum types, do precise exhaustiveness checking
  const EnumType *ET = ScrutineeType->getAs<EnumType>();
  if (ET) {
    const EnumDecl *ED = ET->getDecl();
    if (!ED || ED->isDependentType())
      return;

    // Collect all enumerators
    SmallVector<const EnumConstantDecl *, 8> Enumerators;
    for (const auto *ECD : ED->enumerators())
      Enumerators.push_back(ECD);

    if (Enumerators.empty())
      return;

    // If there's a wildcard without guard, all enumerators are covered
    if (HasWildcardWithoutGuard)
      return;

    // Check which enumerators are covered
    llvm::SmallPtrSet<const EnumConstantDecl *, 8> CoveredEnums;

    for (unsigned I = 0; I < Patterns.size(); ++I) {
      Expr *Pat = Patterns[I].get();
      if (!Pat)
        continue;

      PatternKind Kind = classifyPattern(Pat);
      bool HasGuard = (I < Guards.size() && Guards[I].isUsable() &&
                       Guards[I].get() != nullptr);

      // Wildcard with guard: can't guarantee coverage, skip
      if (Kind == PatternKind::Wildcard && HasGuard)
        continue;

      // Wildcard without guard: covers everything
      if (Kind == PatternKind::Wildcard && !HasGuard) {
        // All covered
        return;
      }

      // Literal pattern: check if it's one of the enum constants
      if (Kind == PatternKind::Literal) {
        if (auto *DRE = dyn_cast<DeclRefExpr>(Pat)) {
          if (auto *ECD = dyn_cast<EnumConstantDecl>(DRE->getDecl())) {
            CoveredEnums.insert(ECD);
          }
        } else if (auto *ME = dyn_cast<MemberExpr>(Pat)) {
          if (auto *ECD = dyn_cast<EnumConstantDecl>(ME->getMemberDecl())) {
            CoveredEnums.insert(ECD);
          }
        } else {
          // Integer literal or other constant — match by value
          if (Pat->isIntegerConstantExpr(Context)) {
            llvm::APSInt Value = Pat->EvaluateKnownConstInt(Context);
            for (const auto *ECD : Enumerators) {
              if (ECD->getInitVal() == Value)
                CoveredEnums.insert(ECD);
            }
          }
        }
      }

      // TypePattern: doesn't cover specific enum values
      // Destructuring: doesn't apply to enum types directly
    }

    // Check for uncovered enumerators
    SmallVector<const EnumConstantDecl *, 8> Uncovered;
    for (const auto *ECD : Enumerators) {
      if (!CoveredEnums.count(ECD))
        Uncovered.push_back(ECD);
    }

    if (!Uncovered.empty()) {
      Diag(MatchLoc, diag::warn_non_exhaustive_match)
          << ScrutineeType;
      for (const auto *ECD : Uncovered) {
        Diag(ECD->getLocation(), diag::note_uncovered_enum_value)
            << ECD;
      }
    }

    return;
  }

  // For bool type, check both true and false are covered
  if (ScrutineeType->isBooleanType()) {
    if (HasWildcardWithoutGuard)
      return;

    bool TrueCovered = false, FalseCovered = false;

    for (unsigned I = 0; I < Patterns.size(); ++I) {
      Expr *Pat = Patterns[I].get();
      if (!Pat)
        continue;

      PatternKind Kind = classifyPattern(Pat);
      bool HasGuard = (I < Guards.size() && Guards[I].isUsable() &&
                       Guards[I].get() != nullptr);

      if (Kind == PatternKind::Wildcard && !HasGuard) {
        TrueCovered = true;
        FalseCovered = true;
        break;
      }

      if (Kind == PatternKind::Wildcard && HasGuard)
        continue; // can't guarantee coverage

      if (Kind == PatternKind::Literal) {
        // CXXBoolLiteralExpr
        if (auto *BoolLit = dyn_cast<CXXBoolLiteralExpr>(Pat)) {
          if (BoolLit->getValue())
            TrueCovered = true;
          else
            FalseCovered = true;
        } else {
          // Integer constant expression — 0 is false, nonzero is true
          if (Pat->isIntegerConstantExpr(Context)) {
            llvm::APSInt Value = Pat->EvaluateKnownConstInt(Context);
            if (Value.getBoolValue())
              TrueCovered = true;
            else
              FalseCovered = true;
          }
        }
      }
    }

    if (!TrueCovered || !FalseCovered) {
      Diag(MatchLoc, diag::warn_non_exhaustive_match) << ScrutineeType;
    }

    return;
  }

  // For integral types with no wildcard, warn that match may be non-exhaustive
  // (we can't enumerate all values, but if there's no wildcard, it's likely
  // incomplete)
  if (ScrutineeType->isIntegralOrEnumerationType() && !HasWildcardWithoutGuard) {
    // Only warn if there are only literal patterns and no wildcard
    bool AllLiteral = true;
    for (unsigned I = 0; I < Patterns.size(); ++I) {
      Expr *Pat = Patterns[I].get();
      if (!Pat)
        continue;
      PatternKind Kind = classifyPattern(Pat);
      bool HasGuard = (I < Guards.size() && Guards[I].isUsable() &&
                       Guards[I].get() != nullptr);
      if (Kind == PatternKind::Wildcard && !HasGuard) {
        AllLiteral = false;
        break;
      }
      if (Kind != PatternKind::Literal && Kind != PatternKind::Unknown) {
        AllLiteral = false;
        break;
      }
    }
    if (AllLiteral && !Patterns.empty()) {
      Diag(MatchLoc, diag::warn_non_exhaustive_match) << ScrutineeType;
    }
  }
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

  // Lower contract_assert/pre/post(condition) to:
  //   if (!(condition)) __builtin_verbose_trap("contract", "message")
  // or if no message:
  //   if (!(condition)) __builtin_trap()
  // The verbose trap provides better diagnostics with the contract category
  // and optional user message.

  // Step 1: Build !(condition)
  ExprResult NegatedCond = ActOnUnaryOp(getCurScope(), Loc, tok::exclaim, Condition);
  if (NegatedCond.isInvalid())
    return StmtError();

  // Step 2: Build the trap call
  ExprResult TrapCall;

  if (Message) {
    // Use __builtin_verbose_trap("contract", message) for better diagnostics
    CXXScopeSpec SS;
    SourceLocation TemplateKWLoc;
    UnqualifiedId TrapName;
    TrapName.setIdentifier(PP.getIdentifierInfo("__builtin_verbose_trap"), Loc);
    ExprResult TrapFn = ActOnIdExpression(
        getCurScope(), SS, TemplateKWLoc, TrapName,
        /*HasTrailingLParen=*/true, /*IsAddressOfOperand=*/false);
    if (TrapFn.isInvalid())
      return StmtError();

    // Build the category string literal: "contract"
    QualType CategoryTy = Context.getConstantArrayType(
        Context.CharTy.withConst(),
        llvm::APInt(32, 9), // strlen("contract") + 1
        nullptr, ArraySizeModifier::Normal, 0);
    StringLiteral *CategorySL = StringLiteral::Create(
        Context, "contract", StringLiteralKind::Ordinary,
        false, CategoryTy, Loc);

    SmallVector<Expr *, 2> TrapArgs;
    TrapArgs.push_back(CategorySL);
    TrapArgs.push_back(Message);

    TrapCall = BuildCallExpr(getCurScope(), TrapFn.get(), Loc, TrapArgs, Loc);
  } else {
    // Use __builtin_trap() when no message provided
    CXXScopeSpec SS;
    SourceLocation TemplateKWLoc;
    UnqualifiedId TrapName;
    TrapName.setIdentifier(PP.getIdentifierInfo("__builtin_trap"), Loc);
    ExprResult TrapFn = ActOnIdExpression(
        getCurScope(), SS, TemplateKWLoc, TrapName,
        /*HasTrailingLParen=*/true, /*IsAddressOfOperand=*/false);
    if (TrapFn.isInvalid())
      return StmtError();

    TrapCall = BuildCallExpr(getCurScope(), TrapFn.get(), Loc, {}, Loc);
  }

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
