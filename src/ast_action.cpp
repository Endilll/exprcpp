#include "exprcpp/ast_action.h"

#include "exprcpp/jit_src_builder.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/Mangle.h>
#include <llvm/Support/Casting.h>
#pragma clang diagnostic pop

namespace exprcpp {

class Name_extractor : public clang::ASTConsumer {
    clang::ASTContext& ctx_;
    clang::MangleContext* mangle_ctx_;

public:
    std::string& user_name;
    std::string& entry_name_mangled;

    Name_extractor(clang::CompilerInstance& ci, std::string& user_name,
                   std::string& entry_name_mangled)
      : ctx_{ci.getASTContext()}
      , mangle_ctx_{ci.getASTContext().createMangleContext()}
      , user_name{user_name}, entry_name_mangled{entry_name_mangled} {}

    bool HandleTopLevelDecl(clang::DeclGroupRef dg) override
    {
        for (const clang::Decl* const decl : dg) {
            if (const auto* const func_decl{
                    llvm::dyn_cast<clang::FunctionDecl>(decl)}) {
                if (ctx_.getFullLoc(func_decl->getLocation())
                        .isInSystemHeader()) {
                    continue;
                }
                llvm::raw_string_ostream os{this->user_name};
                func_decl->printQualifiedName(os);
            } else if (const auto* const ns_decl{
                            llvm::dyn_cast<clang::NamespaceDecl>(decl)}) {
                if (ns_decl->getName() != Jit_src_builder::entry_func_ns) {
                     continue;
                }
                for (const clang::Decl* const inner_decl: ns_decl->decls()) {
                    const auto* const inner_func_decl{
                        llvm::dyn_cast<clang::FunctionDecl>(inner_decl)};
                    if (!inner_func_decl
                        || inner_func_decl->getName()
                           != Jit_src_builder::entry_func_name) {
                        continue;
                    }
                    llvm::raw_string_ostream os{this->entry_name_mangled};
                    mangle_ctx_->mangleCXXName(inner_func_decl, os);
                }
            }

            if (!user_name.empty() && !entry_name_mangled.empty()) {
                return false;
            }
        }

        return true;
    }
};

Name_extractor_action::Name_extractor_action(std::string& user_name,
                                             std::string& entry_name_mangled)
        : user_name{user_name}, entry_name_mangled{entry_name_mangled} {}

std::unique_ptr<clang::ASTConsumer> Name_extractor_action::CreateASTConsumer(
    clang::CompilerInstance& ci, llvm::StringRef)
{
    return std::make_unique<Name_extractor>(ci, this->user_name,
                                            this->entry_name_mangled);
}

bool Name_extractor_action::ParseArgs(const clang::CompilerInstance&,
                                      const std::vector<std::string>&)
{
    return true;
}

clang::PluginASTAction::ActionType Name_extractor_action::getActionType()
{
    return AddBeforeMainAction;
}
} // namespace exprcpp