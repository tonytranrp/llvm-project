//===--- ParseReflect.cpp - C++26 Reflection Parsing ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements parsing for reflection facilities.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/LocInfoType.h"
#include "clang/Basic/DiagnosticParse.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/EnterExpressionEvaluationContext.h"
using namespace clang;

ExprResult Parser::ParseCXXReflectExpression() {
  assert(Tok.is(tok::caretcaret) && "Expected ^^ token");
  SourceLocation CaretCaretLoc = ConsumeToken();
  SourceLocation OperandLoc = Tok.getLocation();

  EnterExpressionEvaluationContext Unevaluated(
      Actions, Sema::ExpressionEvaluationContext::Unevaluated);

  // Case 1: ^^:: (global namespace reflection)
  if (Tok.is(tok::coloncolon)) {
    SourceLocation ColonColonLoc = ConsumeToken();
    return Actions.ActOnCXXReflectGlobalNamespace(CaretCaretLoc,
                                                   ColonColonLoc);
  }

  // Case 2: ^^type-id (type reflection)
  // Try to parse as a type-id first. This handles ^^int, ^^MyClass,
  // ^^std::vector<int>, etc.
  if (isCXXTypeId(TentativeCXXTypeIdContext::AsReflectionOperand)) {
    TypeResult TR = ParseTypeName(/*TypeOf=*/nullptr);
    if (TR.isInvalid())
      return ExprError();

    TypeSourceInfo *TSI = nullptr;
    QualType QT = Actions.GetTypeFromParser(TR.get(), &TSI);

    if (QT.isNull())
      return ExprError();

    if (!TSI)
      TSI = Actions.getASTContext().getTrivialTypeSourceInfo(QT, OperandLoc);

    // Accept all types now (not just builtins)
    return Actions.ActOnCXXReflectExpr(CaretCaretLoc, TSI);
  }

  // Case 3: ^^id-expression (declaration reflection)
  // This handles ^^variable, ^^function_name, ^^ClassName::member, etc.
  if (Tok.is(tok::identifier) || Tok.is(tok::coloncolon) ||
      Tok.is(tok::kw_operator) || Tok.is(tok::tilde) ||
      Tok.is(tok::kw_template)) {
    // Parse as a qualified-id or unqualified-id
    CXXScopeSpec SS;
    if (Tok.is(tok::coloncolon)) {
      ParseOptionalCXXScopeSpecifier(SS, /*ObjectType=*/nullptr,
                                     /*ObjectHasErrors=*/false,
                                     /*EnteringContext=*/false,
                                     /*MayBePseudoDestructor=*/nullptr,
                                     /*IsTypename=*/false,
                                     /*LastLoc=*/nullptr);
    }

    SourceLocation TemplateKWLoc;
    UnqualifiedId Name;
    if (ParseUnqualifiedId(SS, /*ObjectType=*/nullptr,
                           /*ObjectHasErrors=*/false,
                           /*EnteringContext=*/false,
                           /*AllowDestructorName=*/false,
                           /*AllowConstructorName=*/false,
                           /*AllowDeductionGuide=*/false,
                           &TemplateKWLoc, Name)) {
      return ExprError();
    }

    // Look up the name
    SourceLocation NameLoc = Name.getSourceRange().getBegin();
    ExprResult Result = Actions.ActOnCXXReflectExpr(
        CaretCaretLoc, NameLoc,
        /*Decl=*/nullptr); // Sema will resolve the name

    // For now, if we couldn't resolve as a declaration, try type-id fallback
    if (Result.isInvalid()) {
      Diag(OperandLoc, diag::err_cannot_reflect_operand);
    }
    return Result;
  }

  Diag(OperandLoc, diag::err_cannot_reflect_operand);
  return ExprError();
}
