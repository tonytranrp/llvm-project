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

  // Case 2: ^^id-expression (namespace, declaration, or type reflection)
  // Try parsing as an id-expression first. This handles ^^NamespaceName,
  // ^^variable, ^^function_name, ^^NS::member, ^^Outer::Inner, etc.
  // We try this before type-id because namespace names are also valid
  // type-ids in tentative parsing, but we need the id-expression path
  // for namespace reflection to work.
  if (Tok.is(tok::identifier) || Tok.is(tok::coloncolon) ||
      Tok.is(tok::kw_operator) || Tok.is(tok::tilde) ||
      Tok.is(tok::kw_template)) {
    // Parse as a qualified-id or unqualified-id.
    // Always try to parse a scope specifier — this handles both
    // ^^Outer::Inner (leading ::) and ^^Outer::Inner (SS built from
    // the first identifier as a namespace).
    CXXScopeSpec SS;
    ParseOptionalCXXScopeSpecifier(SS, /*ObjectType=*/nullptr,
                                   /*ObjectHasErrors=*/false,
                                   /*EnteringContext=*/false,
                                   /*MayBePseudoDestructor=*/nullptr,
                                   /*IsTypename=*/false,
                                   /*LastLoc=*/nullptr);

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

    // Let Sema figure out what kind of entity the name refers to.
    // It will dispatch to the appropriate ActOnCXXReflectExpr overload
    // (namespace, declaration, or type).
    ExprResult ER = Actions.ActOnCXXReflectExpr(CaretCaretLoc, SS, Name,
                                                 getCurScope());
    if (!ER.isInvalid())
      return ER;
    // If Sema couldn't resolve it as an id-expression (e.g., it's a
    // simple-type-specifier like a class name that wasn't found by lookup),
    // fall through to try type-id parsing.
  }

  // Case 3: ^^type-id (type reflection)
  // This handles ^^int, ^^MyClass, ^^std::vector<int>, etc.
  // Reached when id-expression parsing fails or for built-in types
  // that aren't identifiers.
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

  Diag(OperandLoc, diag::err_cannot_reflect_operand);
  return ExprError();
}

ExprResult Parser::ParseReflectionMetafunction() {
  // Parse: is_type(expr), type_of(expr), identifier_of(expr), decl_of(expr),
  //        name_of(expr), members_of(expr), is_class(expr), is_function(expr),
  //        is_namespace(expr), is_enum(expr), parent_of(expr),
  //        size_of(expr), get_type(expr), is_public(expr), is_private(expr),
  //        is_protected(expr), is_data_member(expr), is_member_function(expr),
  //        is_static(expr), is_inline(expr), is_virtual(expr), is_const(expr),
  //        is_volatile(expr), offset_of(expr), has_parent(expr),
  //        is_template(expr), is_explicit(expr), is_noexcept(expr),
  //        is_constructor(expr), is_destructor(expr), is_empty(expr),
  //        is_enumerator(expr), is_type_alias(expr), is_variable(expr),
  //        is_union(expr), is_struct(expr), has_default_member_initializer(expr),
  //        is_lvalue_reference(expr), is_rvalue_reference(expr),
  //        is_pointer(expr), is_arithmetic(expr), is_abstract(expr),
  //        is_final(expr), is_literal_type(expr), is_signed(expr),
  //        is_unsigned(expr)
  assert((Tok.is(tok::kw_is_type) || Tok.is(tok::kw_type_of) ||
          Tok.is(tok::kw_identifier_of) || Tok.is(tok::kw_decl_of) ||
          Tok.is(tok::kw_name_of) || Tok.is(tok::kw_members_of) ||
          Tok.is(tok::kw_is_class) || Tok.is(tok::kw_is_function) ||
          Tok.is(tok::kw_is_namespace) || Tok.is(tok::kw_is_enum) ||
          Tok.is(tok::kw_parent_of) || Tok.is(tok::kw_size_of) ||
          Tok.is(tok::kw_get_type) || Tok.is(tok::kw_is_public) ||
          Tok.is(tok::kw_is_private) || Tok.is(tok::kw_is_protected) ||
          Tok.is(tok::kw_is_data_member) || Tok.is(tok::kw_is_member_function) ||
          Tok.is(tok::kw_is_static) || Tok.is(tok::kw_is_inline) ||
          Tok.is(tok::kw_is_virtual) || Tok.is(tok::kw_is_const) ||
          Tok.is(tok::kw_is_volatile) || Tok.is(tok::kw_offset_of) ||
          Tok.is(tok::kw_has_parent) || Tok.is(tok::kw_is_template) ||
          Tok.is(tok::kw_is_explicit) || Tok.is(tok::kw_is_noexcept) ||
          Tok.is(tok::kw_is_constructor) || Tok.is(tok::kw_is_destructor) ||
          Tok.is(tok::kw_is_empty) || Tok.is(tok::kw_is_enumerator) ||
          Tok.is(tok::kw_is_type_alias) || Tok.is(tok::kw_is_variable) ||
          Tok.is(tok::kw_is_union) || Tok.is(tok::kw_is_struct) ||
          Tok.is(tok::kw_has_default_member_initializer) ||
          Tok.is(tok::kw_is_lvalue_reference) || Tok.is(tok::kw_is_rvalue_reference) ||
          Tok.is(tok::kw_is_pointer) || Tok.is(tok::kw_is_arithmetic) ||
          Tok.is(tok::kw_is_abstract) || Tok.is(tok::kw_is_final) ||
          Tok.is(tok::kw_is_literal_type) || Tok.is(tok::kw_is_signed) ||
          Tok.is(tok::kw_is_unsigned)) &&
         "Expected reflection metafunction keyword");
  assert(getLangOpts().Reflection && "Reflection not enabled");

  // Save which metafunction keyword before consuming
  tok::TokenKind KwKind = Tok.getKind();
  SourceLocation KwLoc = ConsumeToken();

  if (!Tok.is(tok::l_paren)) {
    Diag(KwLoc, diag::err_expected_after) << KwKind << tok::l_paren;
    return ExprError();
  }

  SourceLocation LParenLoc = ConsumeToken();

  // Parse the argument expression
  ExprResult Arg = ParseAssignmentExpression();
  if (Arg.isInvalid())
    return ExprError();

  if (!Tok.is(tok::r_paren)) {
    Diag(Tok, diag::err_expected) << tok::r_paren;
    return ExprError();
  }
  SourceLocation RParenLoc = ConsumeToken();

  return Actions.ActOnReflectionMetafunction(KwLoc, KwKind, LParenLoc,
                                              Arg.get(), RParenLoc);
}

/// Parse a splice expression: [: expr :]
/// This is the P2996 splice operator that turns a reflection back into
/// a type or expression.
ExprResult Parser::ParseSpliceExpression() {
  assert(Tok.is(tok::l_splice) && "Expected [: for splice");
  SourceLocation LSquareLoc = ConsumeToken(); // consume [:

  // Parse the reflection expression inside the splice
  ExprResult ReflExpr = ParseAssignmentExpression();
  if (ReflExpr.isInvalid())
    return ExprError();

  // Expect :]
  if (!Tok.is(tok::r_splice)) {
    Diag(Tok, diag::err_expected) << tok::r_splice;
    return ExprError();
  }
  SourceLocation RSquareLoc = ConsumeToken(); // consume :]

  return Actions.ActOnSpliceExpression(LSquareLoc, ReflExpr.get(),
                                        RSquareLoc);
}

/// Parse a splice as a type-specifier: [: expr :]
/// This handles [: ^^int :] x; where the splice produces a type.
SourceLocation Parser::ParseSpliceTypeSpecifier(DeclSpec &DS) {
  assert(Tok.is(tok::l_splice) && "Expected [: for splice type specifier");
  SourceLocation LSquareLoc = ConsumeToken(); // consume [:

  // Parse the reflection expression inside the splice
  ExprResult ReflExpr = ParseAssignmentExpression();
  if (ReflExpr.isInvalid()) {
    DS.SetTypeSpecError();
    return LSquareLoc;
  }

  // Expect :]
  if (!Tok.is(tok::r_splice)) {
    Diag(Tok, diag::err_expected) << tok::r_splice;
    DS.SetTypeSpecError();
    return LSquareLoc;
  }
  SourceLocation RSquareLoc = ConsumeToken(); // consume :]

  const char *PrevSpec = nullptr;
  unsigned DiagID;
  const PrintingPolicy &Policy = Actions.getASTContext().getPrintingPolicy();
  if (DS.SetTypeSpecType(DeclSpec::TST_splice, LSquareLoc, PrevSpec,
                         DiagID, ReflExpr.get(), Policy)) {
    Diag(LSquareLoc, DiagID) << PrevSpec;
    DS.SetTypeSpecError();
    return RSquareLoc;
  }

  DS.SetRangeEnd(RSquareLoc);
  return RSquareLoc;
}
