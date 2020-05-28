#pragma once

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <clang/AST/ASTConsumer.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <llvm/ADT/StringRef.h>
#pragma clang diagnostic pop

#include <memory>
#include <string>
#include <vector>

namespace exprcpp {

class Name_extractor_action : public clang::PluginASTAction {
public:
    std::string& user_name;
    std::string& entry_name_mangled;

    Name_extractor_action(std::string& user_name,
                          std::string& entry_name_mangled);

protected:
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& ci, llvm::StringRef) override;

    bool ParseArgs(const clang::CompilerInstance&,
                   const std::vector<std::string>&) override;

    clang::PluginASTAction::ActionType getActionType() override;
};
} // namespace exprcpp