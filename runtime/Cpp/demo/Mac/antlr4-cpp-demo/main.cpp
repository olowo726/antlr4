//
//  main.cpp
//  antlr4-cpp-demo
//
//  Created by Mike Lischke on 13.03.16.
//

#include <iostream>

#include "ANTLRInputStream.h"
#include "CommonTokenStream.h"
#include "TLexer.h"
#include "TParser.h"

#include "TestLexer.h"
#include "TestParser.h"

using namespace antlrcpptest;
using namespace org::antlr::v4::runtime;

int main(int argc, const char * argv[]) {

  /*
  ANTLRInputStream input(L"<foo><conquer>");
  TLexer lexer(&input);
  CommonTokenStream tokens(&lexer);

  TParser parser(&tokens);
  std::shared_ptr<tree::ParseTree> tree = parser.main();
  std::wcout << tree->toStringTree(&parser);
*/

  ANTLRInputStream input(L"ab");
  TestLexer lexer(&input);
  CommonTokenStream tokens(&lexer);

  TestParser parser(&tokens);
  std::shared_ptr<tree::ParseTree> tree = parser.main();
  std::wcout << tree->toStringTree(&parser);

  return 0;
}
