//===--- SILGenLValue.cpp - Constructs logical lvalues for SILGen ---------===//
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

#include "SILGen.h"
#include "swift/AST/AST.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "LValue.h"
#include "ManagedValue.h"
#include "TypeInfo.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace Lowering;

void PathComponent::_anchor() {}
void PhysicalPathComponent::_anchor() {}
void LogicalPathComponent::_anchor() {}

namespace {
  class VarComponent : public PhysicalPathComponent {
    Value address;
  public:
    VarComponent(Value address) : address(address) {
      assert(address.getType()->is<LValueType>() &&
             "var component value must be an address");
    }
    
    Value offset(SILGenFunction &gen, SILLocation loc,
                 Value base) const override {
      assert(!base && "var component must be root of lvalue path");
      return address;
    }
  };
  
  class FragileElementComponent : public PhysicalPathComponent {
    FragileElement element;
  public:
    FragileElementComponent(FragileElement element) : element(element) {}
    
    Value offset(SILGenFunction &gen, SILLocation loc,
                 Value base) const override {
      assert(base && "invalid value for element base");
      assert(base.getType()->is<LValueType>() &&
             "base for element component must be an address");
      LValueType *baseType = base.getType()->castTo<LValueType>();
      return gen.B.createElementAddr(loc, base, element.index,
                                     LValueType::get(element.type,
                                                    baseType->getQualifiers(),
                                                    baseType->getASTContext()));
    }
  };
  
  class GetterSetterComponent : public LogicalPathComponent {
    Value getter;
    Value setter;
    Value subscript;
    
    ManagedValue partialApplyAccessor(SILGenFunction &gen, SILLocation loc,
                                      Value accessor, Value base) const {
      assert((!base || base.getType()->is<LValueType>()) &&
             "base of getter/setter component must be invalid or lvalue");
      gen.B.createRetain(loc, accessor);
      // Apply the base "this" argument, if any.
      ManagedValue appliedThis = base
        ? gen.emitManagedRValueWithCleanup(gen.B.createApply(loc, accessor, base))
        : ManagedValue(accessor);
      // Apply the subscript argument, if any.
      ManagedValue appliedSubscript = subscript
        ? gen.emitManagedRValueWithCleanup(gen.B.createApply(loc,
                                                      appliedThis.forward(gen),
                                                      subscript))
        : ManagedValue(appliedThis);
      return appliedSubscript;
    }
    
  public:
    GetterSetterComponent(Value getter, Value setter,
                          Value subscript = {})
      : getter(getter), setter(setter), subscript(subscript) {
      
      assert(getter && setter &&
             "settable lvalue must have both getter and setter");
    }
    
    void storeRValue(SILGenFunction &gen, SILLocation loc,
                     Value rvalue, Value base,
                     ShouldPreserveValues preserve) const override {
      ManagedValue appliedSetter = partialApplyAccessor(gen, loc,
                                                        setter, base);
      gen.B.createApply(loc, appliedSetter.forward(gen), rvalue);
    }
    
    ManagedValue loadAndMaterialize(SILGenFunction &gen, SILLocation loc,
                                    Value base,
                                    ShouldPreserveValues preserve)
                                    const override
    {
      // FIXME: ignores base and preserve
      ManagedValue appliedGetter = partialApplyAccessor(gen, loc,
                                                        getter, base);
      return gen.emitGetProperty(loc, appliedGetter);
    }
  };
}

LValue SILGenLValue::visitExpr(Expr *e) {
  e->dump();
  llvm_unreachable("unimplemented lvalue expr");
}

LValue SILGenLValue::visitDeclRefExpr(DeclRefExpr *e) {
  LValue lv;
  ValueDecl *decl = e->getDecl();

  // If it's a property, push a reference to the getter and setter.
  if (VarDecl *var = dyn_cast<VarDecl>(decl)) {
    if (var->isProperty()) {
      ManagedValue get = gen.emitConstantRef(e,
                                        SILConstant(var, SILConstant::Getter));
      ManagedValue set = gen.emitConstantRef(e,
                                        SILConstant(var, SILConstant::Setter));
      lv.add<GetterSetterComponent>(get.forward(gen), set.forward(gen));
      return ::std::move(lv);
    }
  }

  // If it's a physical value, push the address.
  Value address = gen.emitReferenceToDecl(e, decl).getUnmanagedValue();
  assert(address.getType()->is<LValueType>() &&
         "physical lvalue decl ref must evaluate to an address");
  lv.add<VarComponent>(address);
  return ::std::move(lv);
}

LValue SILGenLValue::visitMemberRefExpr(MemberRefExpr *e) {
  LValue lv = visit(e->getBase());
  VarDecl *decl = e->getDecl();
  TypeInfo const &ti = gen.getTypeInfo(
                                      e->getBase()->getType()->getRValueType());
  
  if (ti.hasFragileElement(decl->getName())) {
    lv.add<FragileElementComponent>(ti.getFragileElement(decl->getName()));
  } else {
    ManagedValue get = gen.emitConstantRef(e,
                                        SILConstant(decl, SILConstant::Getter));
    ManagedValue set = gen.emitConstantRef(e,
                                        SILConstant(decl, SILConstant::Setter));
    lv.add<GetterSetterComponent>(get.forward(gen), set.forward(gen));
  }
  
  return ::std::move(lv);
}

LValue SILGenLValue::visitTupleElementExpr(TupleElementExpr *e) {
  LValue lv = visit(e->getBase());
  // FIXME: address-only tuples
  lv.add<FragileElementComponent>(FragileElement{e->getType()->getRValueType(),
                                                 e->getFieldNumber()});
  return ::std::move(lv);
}

LValue SILGenLValue::visitAddressOfExpr(AddressOfExpr *e) {
  return visit(e->getSubExpr());
}

LValue SILGenLValue::visitParenExpr(ParenExpr *e) {
  return visit(e->getSubExpr());
}

LValue SILGenLValue::visitRequalifyExpr(RequalifyExpr *e) {
  assert(e->getType()->is<LValueType>() &&
         "non-lvalue requalify in lvalue expression");
  return visit(e->getSubExpr());
}