//===--- ParseType.cpp - Swift Language Parser for Types ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Type Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/Parser.h"
#include "swift/AST/Attr.h"
#include "swift/AST/ExprHandle.h"
#include "swift/AST/TypeLoc.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SaveAndRestore.h"
using namespace swift;

ParserResult<TypeRepr> Parser::parseTypeAnnotation() {
  return parseTypeAnnotation(diag::expected_type);
}

/// parseTypeAnnotation
///   type-annotation:
///     attribute-list type
ParserResult<TypeRepr> Parser::parseTypeAnnotation(Diag<> message) {
  // Parse attributes.
  DeclAttributes attrs;
  parseAttributeList(attrs, true);

  // Parse the type.
  ParserResult<TypeRepr> Ty = parseType(message);
  if (Ty.isNull() || Ty.hasCodeCompletion())
    return Ty;
  return makeParserResult(applyAttributeToType(Ty.getPtrOrNull(), attrs));
}

TypeRepr *Parser::applyAttributeToType(TypeRepr *ty, DeclAttributes &attrs) {
  // Apply those attributes that do apply.
  if (attrs.empty()) return ty;

  return new (Context) AttributedTypeRepr(attrs, ty);
}

ParserResult<TypeRepr> Parser::parseTypeSimple() {
  return parseTypeSimple(diag::expected_type);
}

/// parseTypeSimple
///   type-simple:
///     type-identifier
///     type-tuple
///     type-composition
///     type-simple '.metatype'
///     type-simple '?'
ParserResult<TypeRepr> Parser::parseTypeSimple(Diag<> MessageID) {
  ParserResult<TypeRepr> ty;
  switch (Tok.getKind()) {
  case tok::kw_Self:
  case tok::identifier:
    ty = parseTypeIdentifier();
    break;
  case tok::kw_protocol:
    ty = parseTypeComposition();
    break;
  case tok::l_paren: {
    ty = parseTypeTupleBody();
    break;
  }
  case tok::code_complete: {
    if (CodeCompletion)
      CodeCompletion->completeTypeSimpleBeginning();
    // Eat the code completion token because we handled it.
    consumeToken(tok::code_complete);
    return makeParserCodeCompletionResult<TypeRepr>();
  }
  case tok::kw_super:
  case tok::kw_metatype:
  case tok::kw_self:
  case tok::kw_weak:
  case tok::kw_unowned: {
    // These keywords don't start a decl or a statement, and thus should be
    // safe to skip over.
    diagnose(Tok, MessageID);
    ty = makeParserErrorResult(new (Context) ErrorTypeRepr(Tok.getLoc()));
    consumeToken();
    // FIXME: we could try to continue to parse.
    return ty;
  }
  default:
    diagnose(Tok, MessageID);
    return nullptr;
  }

  // '.metatype' and '?' still leave us with type-simple.
  while (ty.isNonNull()) {
    if ((Tok.is(tok::period) || Tok.is(tok::period_prefix)) &&
        peekToken().is(tok::kw_metatype)) {
      consumeToken();
      SourceLoc metatypeLoc = consumeToken(tok::kw_metatype);
      ty = makeParserResult(ty,
          new (Context) MetaTypeTypeRepr(ty.get(), metatypeLoc));
      continue;
    }
    if (!Tok.isAtStartOfLine() && Tok.is(tok::question_postfix)) {
      ty = parseTypeOptional(ty.get());
      continue;
    }
    break;
  }

  return ty;
}

ParserResult<TypeRepr> Parser::parseType() {
  return parseType(diag::expected_type);
}

/// parseType
///   type:
///     type-function
///     type-array
///
///   type-function:
///     type-simple '->' type
///
ParserResult<TypeRepr> Parser::parseType(Diag<> MessageID) {
  ParserResult<TypeRepr> ty = parseTypeSimple(MessageID);
  if (ty.hasCodeCompletion())
    return makeParserCodeCompletionResult<TypeRepr>();

  if (ty.isNull())
    return nullptr;

  // Handle type-function if we have an arrow.
  if (consumeIf(tok::arrow)) {
    ParserResult<TypeRepr> SecondHalf =
        parseType(diag::expected_type_function_result);
    if (SecondHalf.hasCodeCompletion())
      return makeParserCodeCompletionResult<TypeRepr>();
    if (SecondHalf.isNull())
      return nullptr;
    return makeParserResult(
        new (Context) FunctionTypeRepr(ty.get(), SecondHalf.get()));
  }

  // Parse array types, plus recovery for optional array types.
  // If we see "T[]?", emit a diagnostic; this type must be written "(T[])?"
  // or "Optional<T[]>".
  while (ty.isNonNull() && !Tok.isAtStartOfLine()) {
    if (Tok.is(tok::l_square)) {
      ty = parseTypeArray(ty.get());
    } else if (Tok.is(tok::question_postfix)) {
      if (isa<ArrayTypeRepr>(ty.get())) {
        diagnose(Tok, diag::unsupported_unparenthesized_array_optional)
            .fixItInsert(ty.get()->getStartLoc(), "(")
            .fixItInsert(Lexer::getLocForEndOfToken(SourceMgr,
                                                    ty.get()->getEndLoc()),
                         ")");
      }
      ty = parseTypeOptional(ty.get());
    } else {
      break;
    }
  }

  return ty;
}

ParserResult<TypeRepr> Parser::parseTypeIdentifierWithRecovery(
    Diag<> MessageID, Diag<TypeLoc> NonIdentifierTypeMessageID) {
  ParserResult<TypeRepr> Ty = parseType(MessageID);

  if (!Ty.isParseError() && !isa<IdentTypeRepr>(Ty.get())) {
    diagnose(Ty.get()->getStartLoc(), NonIdentifierTypeMessageID, Ty.get())
        .highlight(Ty.get()->getSourceRange());
    Ty.setIsParseError();
    Ty = makeParserResult(
        Ty, new (Context) ErrorTypeRepr(Ty.get()->getSourceRange()));
  }

  assert(Ty.isNull() ||
         isa<IdentTypeRepr>(Ty.get()) ||
         isa<ErrorTypeRepr>(Ty.get()));
  return Ty;
}

bool Parser::parseGenericArguments(SmallVectorImpl<TypeRepr*> &Args,
                                   SourceLoc &LAngleLoc,
                                   SourceLoc &RAngleLoc) {
  // Parse the opening '<'.
  assert(startsWithLess(Tok) && "Generic parameter list must start with '<'");
  LAngleLoc = consumeStartingLess();

  do {
    ParserResult<TypeRepr> Ty = parseType(diag::expected_type);
    if (Ty.isNull() || Ty.hasCodeCompletion()) {
      // Skip until we hit the '>'.
      skipUntilGreaterInTypeList();
      if (startsWithGreater(Tok))
        consumeStartingGreater();
      return true;
    }

    Args.push_back(Ty.get());
    // Parse the comma, if the list continues.
  } while (consumeIf(tok::comma));

  if (!startsWithGreater(Tok)) {
    diagnose(Tok, diag::expected_rangle_generic_arg_list);
    diagnose(LAngleLoc, diag::opening_angle);

    // Skip until we hit the '>'.
    skipUntilGreaterInTypeList();
    if (startsWithGreater(Tok))
      RAngleLoc = consumeStartingGreater();
    return true;
  } else {
    RAngleLoc = consumeStartingGreater();
  }

  return false;
}

/// parseTypeIdentifier
///   
///   type-identifier:
///     identifier generic-args? ('.' identifier generic-args?)*
///
ParserResult<IdentTypeRepr> Parser::parseTypeIdentifier() {
  if (Tok.isNot(tok::identifier) && Tok.isNot(tok::kw_Self)) {
    if (Tok.is(tok::code_complete)) {
      if (CodeCompletion)
        CodeCompletion->completeTypeIdentifierWithDot(nullptr);
      // Eat the code completion token because we handled it.
      consumeToken(tok::code_complete);
      return makeParserCodeCompletionResult<IdentTypeRepr>();
    }

    diagnose(Tok, diag::expected_identifier_for_type);
    return nullptr;
  }

  ParserStatus Status;
  SmallVector<IdentTypeRepr::Component, 4> ComponentsR;
  SourceLoc EndLoc;
  while (true) {
    SourceLoc Loc;
    Identifier Name;
    switch (Tok.getKind()) {
    case tok::kw_Self:
      Name = Context.getIdentifier(Tok.getText());
      Loc = Tok.getLoc();
      consumeToken();
      break;

    // FIXME: specialize diagnostic for 'metatype': type can not start with
    // 'metatype'
    // FIXME: offer a fixit: 'self' -> 'Self'
    default:
      if (parseIdentifier(Name, Loc, diag::expected_identifier_in_dotted_type))
        Status.setIsParseError();
      break;
    }

    if (Loc.isValid()) {
      SourceLoc LAngle, RAngle;
      SmallVector<TypeRepr*, 8> GenericArgs;
      if (startsWithLess(Tok)) {
        if (parseGenericArguments(GenericArgs, LAngle, RAngle))
          return nullptr;
      }
      EndLoc = Loc;

      ComponentsR.push_back(IdentTypeRepr::Component(
          Loc, Name, Context.AllocateCopy(GenericArgs), CurDeclContext));
    }

    // Treat 'Foo.<anything>' as an attempt to write a dotted type
    // unless <anything> is 'metatype'.
    if ((Tok.is(tok::period) || Tok.is(tok::period_prefix))) {
      if (peekToken().is(tok::code_complete)) {
        Status.setHasCodeCompletion();
        break;
      }
      if (peekToken().isNot(tok::kw_metatype)) {
        consumeToken();
        continue;
      }
    } else if (Tok.is(tok::code_complete)) {
      Status.setHasCodeCompletion();
      break;
    }
    break;
  }

  IdentTypeRepr *ITR = nullptr;
  if (ComponentsR.size() != 0) {
    // Lookup element #0 through our current scope chains in case it is some thing
    // local (this returns null if nothing is found).
    if (auto Entry = lookupInScope(ComponentsR[0].getIdentifier()))
      ComponentsR[0].setValue(Entry);

    ITR = IdentTypeRepr::create(Context, ComponentsR);
  }

  if (Status.hasCodeCompletion() && CodeCompletion) {
    if (Tok.isNot(tok::code_complete)) {
      // We have a dot.
      consumeToken();
      CodeCompletion->completeTypeIdentifierWithDot(ITR);
    } else {
      CodeCompletion->completeTypeIdentifierWithoutDot(ITR);
    }
    // Eat the code completion token because we handled it.
    consumeToken(tok::code_complete);
  }

  return makeParserResult(Status, ITR);
}

/// parseTypeComposition
///   
///   type-composition:
///     'protocol' '<' type-composition-list? '>'
///
///   type-composition-list:
///     type-identifier (',' type-identifier)*
///
ParserResult<ProtocolCompositionTypeRepr> Parser::parseTypeComposition() {
  SourceLoc ProtocolLoc = consumeToken(tok::kw_protocol);
 
  // Check for the starting '<'.
  if (!startsWithLess(Tok)) {
    diagnose(Tok, diag::expected_langle_protocol);
    return nullptr;
  }
  SourceLoc LAngleLoc = consumeStartingLess();
  
  // Check for empty protocol composition.
  if (startsWithGreater(Tok)) {
    SourceLoc RAngleLoc = consumeStartingGreater();
    return makeParserResult(new (Context) ProtocolCompositionTypeRepr(
                                             MutableArrayRef<IdentTypeRepr *>(),
                                             ProtocolLoc,
                                             SourceRange(LAngleLoc, RAngleLoc)));
  }
  
  // Parse the type-composition-list.
  ParserStatus Status;
  SmallVector<IdentTypeRepr *, 4> Protocols;
  do {
    // Parse the type-identifier.
    ParserResult<IdentTypeRepr> Protocol = parseTypeIdentifier();
    Status |= Protocol;
    if (Protocol.isNonNull())
      Protocols.push_back(Protocol.get());
  } while (consumeIf(tok::comma));
  
  // Check for the terminating '>'.
  SourceLoc EndLoc = Tok.getLoc();
  if (!startsWithGreater(Tok)) {
    if (Status.isSuccess()) {
      diagnose(Tok, diag::expected_rangle_protocol);
      diagnose(LAngleLoc, diag::opening_angle);
      Status.setIsParseError();
    }

    // Skip until we hit the '>'.
    skipUntilGreaterInTypeList();
    if (startsWithGreater(Tok))
      EndLoc = consumeStartingGreater();    
  } else {
    EndLoc = consumeStartingGreater();
  }

  return makeParserResult(Status, ProtocolCompositionTypeRepr::create(
      Context, Protocols, ProtocolLoc, SourceRange(LAngleLoc, EndLoc)));
}

/// parseTypeTupleBody
///   type-tuple:
///     '(' type-tuple-body? ')'
///   type-tuple-body:
///     type-tuple-element (',' type-tuple-element)* '...'?
///   type-tuple-element:
///     identifier ':' type-annotation
///     type-annotation
ParserResult<TupleTypeRepr> Parser::parseTypeTupleBody() {
  SourceLoc RPLoc, LPLoc = consumeToken(tok::l_paren);
  SourceLoc EllipsisLoc;
  SmallVector<TypeRepr *, 8> ElementsR;
  bool HadEllipsis = false;

  ParserStatus Status = parseList(tok::r_paren, LPLoc, RPLoc,
                                  tok::comma, /*OptionalSep=*/false,
                                  diag::expected_rparen_tuple_type_list,
                                  [&] () -> ParserStatus {
    // If the tuple element starts with "ident :", then
    // the identifier is an element tag, and it is followed by a type
    // annotation.
    if (isStartOfBindingName(Tok) && peekToken().is(tok::colon)) {
      // Consume the name
      // FIXME: Should the identifier '_' ever be formed?
      Identifier name = Context.getIdentifier(Tok.getText());
      SourceLoc nameLoc = consumeToken();

      // Consume the ':'.
      consumeToken(tok::colon);

      // Parse the type annotation.
      ParserResult<TypeRepr> type = parseTypeAnnotation(diag::expected_type);
      if (type.hasCodeCompletion())
        return makeParserCodeCompletionStatus();
      if (type.isNull())
        return makeParserError();

      ElementsR.push_back(
          new (Context) NamedTypeRepr(name, type.get(), nameLoc));
    } else {
      // Otherwise, this has to be a type.
      ParserResult<TypeRepr> type = parseTypeAnnotation();
      if (type.hasCodeCompletion())
        return makeParserCodeCompletionStatus();
      if (type.isNull())
        return makeParserError();
      ElementsR.push_back(type.get());
    }

    // Parse '= expr' here so we can complain about it directly, rather
    // than dying when we see it.
    if (Tok.is(tok::equal)) {
      SourceLoc equalLoc = consumeToken(tok::equal);
      auto init = parseExpr(diag::expected_init_value);
      auto inFlight = diagnose(equalLoc, diag::tuple_type_init);
      if (init.isNonNull())
        inFlight.fixItRemove(SourceRange(equalLoc, init.get()->getEndLoc()));
    }

    if (Tok.is(tok::ellipsis)) {
      EllipsisLoc = consumeToken(tok::ellipsis);
      if (Tok.is(tok::r_paren)) {
        HadEllipsis = true;
      } else {
        diagnose(EllipsisLoc, diag::unexpected_ellipsis_in_tuple);
        return makeParserError();
      }
    }
    return makeParserSuccess();
  });

  if (HadEllipsis && ElementsR.empty()) {
    diagnose(EllipsisLoc, diag::empty_tuple_ellipsis);
    return nullptr;
  }

  return makeParserResult(Status, TupleTypeRepr::create(
      Context, ElementsR, SourceRange(LPLoc, RPLoc),
      HadEllipsis ? EllipsisLoc : SourceLoc()));
}


/// parseTypeArray - Parse the type-array production, given that we
/// are looking at the initial l_square.  Note that this index
/// clause is actually the outermost (first-indexed) clause.
///
///   type-array:
///     type-simple
///     type-array '[' ']'
///     type-array '[' expr ']'
///
ParserResult<ArrayTypeRepr> Parser::parseTypeArray(TypeRepr *Base) {
  assert(Tok.isFollowingLSquare());
  SourceLoc lsquareLoc = consumeToken();

  // Handle the [] production, meaning an array slice.
  if (Tok.is(tok::r_square)) {
    SourceLoc rsquareLoc = consumeToken(tok::r_square);
    ParserResult<TypeRepr> NestedType = makeParserResult(Base);

    // If we're starting another square-bracket clause, recur.
    if (Tok.isFollowingLSquare()) {
      NestedType = parseTypeArray(Base);
      if (NestedType.hasCodeCompletion())
        return makeParserCodeCompletionResult<ArrayTypeRepr>();
      if (NestedType.isNull()) {
        // We could not parse the rest of the type, but we still have the base
        // type.
        NestedType = makeParserErrorResult(Base);
      }
    }

    // Just build a normal array slice type.
    auto ATR = new (Context) ArrayTypeRepr(
        NestedType.get(), nullptr, SourceRange(lsquareLoc, rsquareLoc));
    if (NestedType.isParseError())
      return makeParserErrorResult(ATR);
    else
      return makeParserResult(ATR);
  }

  ParserResult<Expr> sizeEx = parseExpr(diag::expected_expr_array_type);
  if (sizeEx.hasCodeCompletion())
    return makeParserCodeCompletionStatus();
  if (sizeEx.isNull()) {
    consumeIf(tok::r_square);
    // FIXME: better recovery.
    return nullptr;
  }

  SourceLoc rsquareLoc;
  if (parseMatchingToken(tok::r_square, rsquareLoc,
                         diag::expected_rbracket_array_type, lsquareLoc))
    return nullptr;

  // If we're starting another square-bracket clause, recur.
  if (Tok.isFollowingLSquare() && parseTypeArray(Base).isParseError())
    return nullptr;
  
  // FIXME: We don't supported fixed-length arrays yet.
  diagnose(lsquareLoc, diag::unsupported_fixed_length_array)
    .highlight(sizeEx.get()->getSourceRange());
  
  return nullptr;
}

/// Parse a single optional suffix, given that we are looking at the
/// question mark.
ParserResult<OptionalTypeRepr> Parser::parseTypeOptional(TypeRepr *base) {
  assert(Tok.is(tok::question_postfix));
  SourceLoc questionLoc = consumeToken();
  return makeParserResult(new (Context) OptionalTypeRepr(base, questionLoc));
}

//===--------------------------------------------------------------------===//
// Speculative type list parsing
//===--------------------------------------------------------------------===//

static bool isGenericTypeDisambiguatingToken(Token &tok) {
  switch (tok.getKind()) {
  default:
    return false;
  case tok::r_paren:
  case tok::r_square:
  case tok::l_brace:
  case tok::r_brace:
  case tok::period:
  case tok::comma:
  case tok::semi:
  case tok::eof:
    return true;
  
  case tok::period_prefix:
    // These will be turned into following tokens if they appear unspaced after
    // a generic angle bracket.
    return tok.getText().data()[-1] == '>';
      
  case tok::l_paren:
  case tok::l_square:
    // These only apply to the generic type if they don't start a new line.
    return !tok.isAtStartOfLine();
  }
}

bool Parser::canParseAsGenericArgumentList() {
  if (!Tok.isAnyOperator() || !Tok.getText().equals("<"))
    return false;

  BacktrackingScope backtrack(*this);

  if (canParseGenericArguments())
    return isGenericTypeDisambiguatingToken(Tok);

  return false;
}

bool Parser::canParseGenericArguments() {
  // Parse the opening '<'.
  if (!startsWithLess(Tok))
    return false;
  consumeStartingLess();
  
  do {
    if (!canParseType())
      return false;
    // Parse the comma, if the list continues.
  } while (consumeIf(tok::comma));
  
  if (!startsWithGreater(Tok)) {
    return false;
  } else {
    consumeStartingGreater();
    return true;
  }
}

bool Parser::canParseType() {
  switch (Tok.getKind()) {
  case tok::kw_Self:
  case tok::identifier:
    if (!canParseTypeIdentifier())
      return false;
    break;
  case tok::kw_protocol:
    if (!canParseTypeComposition())
      return false;
    break;
  case tok::l_paren: {
    consumeToken();
    if (!canParseTypeTupleBody())
      return false;
    break;
  }
  default:
    return false;
  }
  
  // '.metatype' and '?' still leave us with type-simple.
  while (true) {
    if ((Tok.is(tok::period) || Tok.is(tok::period_prefix)) &&
        peekToken().is(tok::kw_metatype)) {
      consumeToken();
      consumeToken(tok::kw_metatype);
      continue;
    }
    if (Tok.is(tok::question_postfix)) {
      consumeToken();
      continue;
    }
    break;
  }
  
  // Handle type-function if we have an arrow.
  if (consumeIf(tok::arrow)) {
    if (!canParseType())
      return false;
    return true;
  }

  // Handle optional types and arrays.
  // For recovery purposes, accept "T[]?" here, even though we'll reject it
  // later.
  while (!Tok.isAtStartOfLine()) {
    if (Tok.is(tok::l_square)) {
      if (!canParseTypeArray())
        return false;
    } else if (Tok.is(tok::question_postfix)) {
      consumeToken();
    } else {
      break;
    }
  }

  return true;
}

bool Parser::canParseTypeIdentifier() {
  if (Tok.isNot(tok::identifier) && Tok.isNot(tok::kw_Self)) {
    return false;
  }
  
  while (true) {
    switch (Tok.getKind()) {
    case tok::identifier:
      consumeToken();
      break;
    default:
      return false;
    }
    
    if (startsWithLess(Tok)) {
      if (!canParseGenericArguments())
        return false;
    }

    // Treat 'Foo.<anything>' as an attempt to write a dotted type
    // unless <anything> is 'metatype'.
    if ((Tok.is(tok::period) || Tok.is(tok::period_prefix)) &&
        peekToken().isNot(tok::kw_metatype)) {
      consumeToken();
    } else {
      return true;
    }
  }
}

bool Parser::canParseTypeComposition() {
  consumeToken(tok::kw_protocol);
  
  // Check for the starting '<'.
  if (!startsWithLess(Tok)) {
    return false;
  }
  consumeStartingLess();
  
  // Check for empty protocol composition.
  if (startsWithGreater(Tok)) {
    consumeStartingGreater();
    return true;
  }
  
  // Parse the type-composition-list.
  do {
    if (!canParseTypeIdentifier()) {
      return false;
    }
  } while (consumeIf(tok::comma));
  
  // Check for the terminating '>'.
  if (!startsWithGreater(Tok)) {
    return false;
  }
  consumeStartingGreater();
  
  return true;
}

bool Parser::canParseTypeTupleBody() {
  if (Tok.isNot(tok::r_paren) && Tok.isNot(tok::r_brace) &&
      Tok.isNot(tok::ellipsis) && !isStartOfDecl(Tok, peekToken())) {
    do {
      // If the tuple element starts with "ident :", then it is followed
      // by a type annotation.
      if (Tok.is(tok::identifier) && peekToken().is(tok::colon)) {
        consumeToken(tok::identifier);
        consumeToken(tok::colon);

        // Parse attributes.
        if (consumeIf(tok::l_square)) {
          while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
                 Tok.isNot(tok::r_square) && Tok.isNot(tok::r_paren) &&
                 !isStartOfDecl(Tok, peekToken()))
            skipSingle();

          if (!consumeIf(tok::r_square))
            return false;
        }

        // Parse the type.
        if (!canParseType())
          return false;

        // Parse default values. This aren't actually allowed, but we recover
        // better if we skip over them.
        if (consumeIf(tok::equal)) {
          while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_paren) &&
                 Tok.isNot(tok::r_brace) && Tok.isNot(tok::ellipsis) &&
                 Tok.isNot(tok::comma) &&
                 !isStartOfDecl(Tok, peekToken())) {
            skipSingle();
          }
        }

        continue;
      }
      
      // Otherwise, this has to be a type.

      // Parse attributes.
      if (consumeIf(tok::l_square)) {
        while (Tok.isNot(tok::eof) && Tok.isNot(tok::r_brace) &&
               Tok.isNot(tok::r_square) && Tok.isNot(tok::r_paren) &&
               !isStartOfDecl(Tok, peekToken()))
          skipSingle();

        if (!consumeIf(tok::r_square))
          return false;
      }

      if (!canParseType())
        return false;
    } while (consumeIf(tok::comma));
  }
  
  if (Tok.is(tok::ellipsis)) {
    consumeToken();
  }
  
  if (Tok.is(tok::r_paren)) {
    consumeToken();
    return true;
  } else {
    return false;
  }
}


bool Parser::canParseTypeArray() {
  assert(Tok.isFollowingLSquare());
  consumeToken();
  
  // Handle the [] production, meaning an array slice.
  if (Tok.is(tok::r_square)) {
    consumeToken(tok::r_square);
    
    // If we're starting another square-bracket clause, recur.
    if (Tok.isFollowingLSquare())
      return canParseTypeArray();
    
    return true;
  }
  
  // FIXME: Size expressions!
  return false;
}

