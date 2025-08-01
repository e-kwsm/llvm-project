// RUN: %clang_cc1 -std=c++03 -verify -ast-dump %s > %t-03
// RUN: FileCheck --input-file=%t-03 %s
// RUN: %clang_cc1 -std=c++11 -verify -ast-dump %s > %t-11
// RUN: FileCheck --input-file=%t-11 %s
// RUN: FileCheck --input-file=%t-11 %s --check-prefix=CHECK-CXX11
// RUN: %clang_cc1 -verify -std=c++17 %s

// http://llvm.org/PR7905
namespace PR7905 {
struct S; // expected-note {{forward declaration}}
void foo1() {
  (void)(S[]) {{3}}; // expected-error {{array has incomplete element type}}
}

template <typename T> struct M { T m; };
void foo2() {
  (void)(M<short> []) {{3}};
}
}

// Check compound literals mixed with C++11 list-initialization.
namespace brace_initializers {
  struct POD {
    int x, y;
  };
  struct HasCtor {
    HasCtor(int x, int y);
  };
  struct HasDtor {
    int x, y;
    ~HasDtor();
  };
  struct HasCtorDtor {
    HasCtorDtor(int x, int y);
    ~HasCtorDtor();
  };

  POD p = (POD){1, 2};
  // CHECK-NOT: CXXBindTemporaryExpr {{.*}} 'brace_initializers::POD'
  // CHECK: CompoundLiteralExpr {{.*}} 'POD':'brace_initializers::POD'
  // CHECK-NEXT: InitListExpr {{.*}} 'POD':'brace_initializers::POD'
  // CHECK-NEXT: ConstantExpr {{.*}}
  // CHECK-NEXT: IntegerLiteral {{.*}} 1{{$}}
  // CHECK-NEXT: ConstantExpr {{.*}}
  // CHECK-NEXT: IntegerLiteral {{.*}} 2{{$}}

  void test() {
    (void)(POD){1, 2};
    // CHECK-NOT: CXXBindTemporaryExpr {{.*}} 'POD':'brace_initializers::POD'
    // CHECK-NOT: ConstantExpr {{.*}} 'POD':'brace_initializers::POD'
    // CHECK: CompoundLiteralExpr {{.*}} 'POD':'brace_initializers::POD'
    // CHECK-NEXT: InitListExpr {{.*}} 'POD':'brace_initializers::POD'
    // CHECK-NEXT: IntegerLiteral {{.*}} 1{{$}}
    // CHECK-NEXT: IntegerLiteral {{.*}} 2{{$}}

    (void)(HasDtor){1, 2};
    // CHECK: CXXBindTemporaryExpr {{.*}} 'HasDtor':'brace_initializers::HasDtor'
    // CHECK-NEXT: CompoundLiteralExpr {{.*}} 'HasDtor':'brace_initializers::HasDtor'
    // CHECK-NEXT: InitListExpr {{.*}} 'HasDtor':'brace_initializers::HasDtor'
    // CHECK-NEXT: IntegerLiteral {{.*}} 1{{$}}
    // CHECK-NEXT: IntegerLiteral {{.*}} 2{{$}}

#if __cplusplus >= 201103L
    (void)(HasCtor){1, 2};
    // CHECK-CXX11-NOT: CXXBindTemporaryExpr {{.*}} 'HasCtor':'brace_initializers::HasCtor'
    // CHECK-CXX11-NOT: ConstantExpr {{.*}} 'HasCtor':'brace_initializers::HasCtor'
    // CHECK-CXX11: CompoundLiteralExpr {{.*}} 'HasCtor':'brace_initializers::HasCtor'
    // CHECK-CXX11-NEXT: CXXTemporaryObjectExpr {{.*}} 'HasCtor':'brace_initializers::HasCtor'
    // CHECK-CXX11-NEXT: IntegerLiteral {{.*}} 1{{$}}
    // CHECK-CXX11-NEXT: IntegerLiteral {{.*}} 2{{$}}

    (void)(HasCtorDtor){1, 2};
    // CHECK-CXX11: CXXBindTemporaryExpr {{.*}} 'HasCtorDtor':'brace_initializers::HasCtorDtor'
    // CHECK-CXX11-NOT: ConstantExpr {{.*}} 'HasCtorDtor':'brace_initializers::HasCtorDtor'
    // CHECK-CXX11: CompoundLiteralExpr {{.*}} 'HasCtorDtor':'brace_initializers::HasCtorDtor'
    // CHECK-CXX11-NEXT: CXXTemporaryObjectExpr {{.*}} 'HasCtorDtor':'brace_initializers::HasCtorDtor'
    // CHECK-CXX11-NEXT: IntegerLiteral {{.*}} 1{{$}}
    // CHECK-CXX11-NEXT: IntegerLiteral {{.*}} 2{{$}}
#endif
  }

  struct PrivateDtor {
    int x, y;
  private:
    ~PrivateDtor(); // expected-note {{declared private here}}
  };

  void testPrivateDtor() {
    (void)(PrivateDtor){1, 2}; // expected-error {{temporary of type 'PrivateDtor' has private destructor}}
  }
}

// This doesn't necessarily need to be an error, but CodeGen can't handle it
// at the moment.
int PR17415 = (int){PR17415}; // expected-error {{initializer element is not a compile-time constant}}

// Make sure we accept this.  (Not sure if we actually should... but we do
// at the moment.)
template<unsigned> struct Value { };
template<typename T>
int &check_narrowed(Value<sizeof((T){1.1})>);

#if __cplusplus >= 201103L
// Compound literals in global lambdas have automatic storage duration
// and are not subject to the constant-initialization rules.
int computed_with_lambda = [] {
  int x = 5;
  int result = ((int[]) { x, x + 2, x + 4, x + 6 })[0];
  return result;
}();
#endif

namespace DynamicFileScopeLiteral {
// This covers the case where we have a file-scope compound literal with a
// non-constant initializer in C++. Previously, we had a bug where Clang forgot
// to consider initializer list elements for bases.
struct Empty {};
struct Foo : Empty { // expected-note 0+ {{candidate constructor}}
  int x;
  int y;
};
int f();
#if __cplusplus < 201103L
// expected-error@+6 {{non-aggregate type 'Foo' cannot be initialized with an initializer list}}
#elif __cplusplus < 201703L
// expected-error@+4 {{no matching constructor}}
#else
// expected-error@+2 {{initializer element is not a compile-time constant}}
#endif
Foo o = (Foo){ {}, 1, f() };
}

#if __cplusplus >= 201103L
namespace GH147949 {
  // Make sure we handle transparent InitListExprs correctly.
  struct S { int x : 3; };
  const S* x = (const S[]){S{S{3}}};
}
#endif
