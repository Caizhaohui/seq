#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lang/seq.h"
#include "parser/ast/codegen.h"
#include "parser/ast/codegen_ctx.h"
#include "parser/ast/doc.h"
#include "parser/ast/format.h"
#include "parser/ast/transform.h"
#include "parser/ast/transform_ctx.h"
#include "parser/ocaml.h"
#include "parser/parser.h"
#include "util/fmt/format.h"

using std::make_shared;
using std::string;
using std::vector;

int __level__ = 0;
int __dbg_level__ = 0;

namespace seq {

void generateDocstr(const std::string &file) {
  LOG("DOC MODE! {}", 1);
  // ast::DocStmtVisitor d;
  // ast::parse_file(file)->accept(d);
}

seq::SeqModule *parse(const std::string &argv0, const std::string &file, bool isCode,
                      bool isTest) {
  try {
    auto d = getenv("SEQ_DEBUG");
    if (d)
      __dbg_level__ = strtol(d, nullptr, 10);
    // auto stmts = isCode ? ast::parse_code(argv0, file) :
    // ast::parse_file(file);

    vector<string> cases;
    string line, current;
    std::ifstream fin(file);
    while (std::getline(fin, line)) {
      if (line == "--") {
        cases.push_back(current);
        current = "";
      } else
        current += line + "\n";
    }
    if (current.size())
      cases.push_back(current);
    FILE *fo = fopen("tmp/out.htm", "w");
    seq::SeqModule *module;

    int st = 0, lim = 1000;
    for (int ci = st, ii = 0; ci < cases.size() && ii < lim; ci++, ii++) {
      LOG3("[[[ case {} ]]]", ci);
      char abs[PATH_MAX + 1];
      realpath(file.c_str(), abs);
      auto stmts = ast::parseCode(abs, cases[ci]);
      auto ctx = ast::TypeContext::getContext(argv0, abs);
      auto tv = ast::TransformVisitor(ctx).realizeBlock(stmts.get(), false);

      LOG3("--- Done with typecheck ---");
      LOG3("{}", ast::FormatVisitor::format(ctx, tv, false, true));
      module = new seq::SeqModule();
      module->setFileName(abs);
      auto lctx = ast::LLVMContext::getContext(abs, ctx, module);
      ast::CodegenVisitor(lctx).transform(tv.get());
      LOG3("--- Done with codegen ---");
      module->execute({}, {});
      // return module;

      fmt::print(fo, "-------------------------------<hr/>\n");
      LOG("--");
    }
    fclose(fo);
    exit(0);

    // auto cache = make_shared<ast::ImportCache>(argv0);
    // auto stdlib = make_shared<ast::Context>(cache, module->getBlock(),
    // module,
    // nullptr, "");
    // stdlib->loadStdlib(module->getArgVar());
    // auto context =
    // make_shared<ast::Context>(cache,
    // module->getBlock(), module,
    //  nullptr, file);
    // ast::CodegenStmtVisitor(*context).transform(tv);
    return module;
  } catch (seq::exc::SeqException &e) {
    if (isTest) {
      throw;
    }
    seq::compilationError(e.what(), e.getSrcInfo().file, e.getSrcInfo().line,
                          e.getSrcInfo().col);
    return nullptr;
  } catch (seq::exc::ParserException &e) {
    if (isTest)
      throw;
    for (int i = 0; i < e.messages.size(); i++)
      compilationMessage("\033[1;31merror:\033[0m", e.messages[i], e.locations[i].file,
                         e.locations[i].line, e.locations[i].col);
    exit(EXIT_FAILURE);
    return nullptr;
  }
}

void execute(seq::SeqModule *module, vector<string> args, vector<string> libs,
             bool debug) {
  config::config().debug = debug;
  // try {
  module->execute(args, libs);
  // }
  // catch (exc::SeqException &e) {
  //   compilationError(e.what(), e.getSrcInfo().file, e.getSrcInfo().line,
  //                    e.getSrcInfo().col);
  // }
}

void compile(seq::SeqModule *module, const string &out, bool debug) {
  config::config().debug = debug;
  try {
    module->compile(out);
  } catch (exc::SeqException &e) {
    compilationError(e.what(), e.getSrcInfo().file, e.getSrcInfo().line,
                     e.getSrcInfo().col);
  }
}

} // namespace seq
