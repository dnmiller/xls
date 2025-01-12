// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef XLS_NETLIST_NETLIST_PARSER_H_
#define XLS_NETLIST_NETLIST_PARSER_H_

#include <sys/types.h>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/netlist/netlist.h"

namespace xls {
namespace netlist {
namespace rtl {

// Kinds of tokens the scanner emits.
enum class TokenKind {
  kStartParams,   // #(
  kOpenParen,     // (
  kCloseParen,    // )
  kOpenBracket,   // [
  kCloseBracket,  // ]
  kOpenBrace,     // {
  kCloseBrace,    // }
  kDot,
  kComma,
  kColon,
  kSemicolon,
  kEquals,
  kQuote,
  kName,
  kNumber,
};

// Returns a string representation of "kind" suitable for debugging.
std::string TokenKindToString(TokenKind kind);

// Represents a position in input text.
struct Pos {
  int64_t lineno;
  int64_t colno;

  std::string ToHumanString() const;
};

// Represents a scanned token (that comes from scanning a character stream).
struct Token {
  TokenKind kind;
  Pos pos;
  std::string value;

  std::string ToString() const;
};

// Token scanner for netlist files.
class Scanner {
 public:
  explicit Scanner(absl::string_view text) : text_(text) {}

  absl::StatusOr<Token> Peek();

  absl::StatusOr<Token> Pop();

  bool AtEof() {
    DropIgnoredChars();
    return index_ >= text_.size();
  }

 private:
  absl::StatusOr<Token> ScanName(char startc, Pos pos, bool is_escaped);
  absl::StatusOr<Token> ScanNumber(char startc, Pos pos);
  absl::StatusOr<Token> PeekInternal();

  // Drops any characters that should not be converted to Tokens, including
  // whitespace, comments, and attributes.
  // Note that we may eventually want to expose attributes to the Parser, but
  // until then it's much simpler to treat attributes like block comments and
  // ignore everything inside of them. This also means that the Scanner will
  // accept attributes that are in invalid positions.
  void DropIgnoredChars();

  char PeekCharOrDie() const;
  char PeekChar2OrDie() const;
  char PopCharOrDie();
  void DropCharOrDie() { (void)PopCharOrDie(); }
  Pos GetPos() const { return Pos{lineno_, colno_}; }

  // Internal version of EOF checking that doesn't attempt to discard the
  // comments/whitespace as the public AtEof() does above -- this simply checks
  // whether the character stream index has reached the end of the text.
  bool AtEofInternal() const { return index_ >= text_.size(); }

  absl::string_view text_;
  int64_t index_ = 0;
  int64_t lineno_ = 0;
  int64_t colno_ = 0;
  absl::optional<Token> lookahead_;
};

class Parser {
 public:
  // Parses a netlist with the given cell library and token scanner.
  // Returns a status on parse error.
  static absl::StatusOr<std::unique_ptr<Netlist>> ParseNetlist(
      CellLibrary* cell_library, Scanner* scanner);

 private:
  explicit Parser(CellLibrary* cell_library, Scanner* scanner)
      : cell_library_(cell_library), scanner_(scanner) {}

  // Parses a cell instantiation (e.g. in module scope).
  absl::Status ParseInstance(Module* module, Netlist& netlist);

  // Parses a cell module name out of the token stream and returns the
  // corresponding CellLibraryEntry for that module name.
  absl::StatusOr<const CellLibraryEntry*> ParseCellModule(Netlist& netlist);

  // Parses a wire declaration at the module scope.
  absl::Status ParseNetDecl(Module* module, NetDeclKind kind);

  struct Range {
    int64_t high;
    int64_t low;
  };

  // Parses an assign declaration at the module scope.
  absl::Status ParseAssignDecl(Module *module);
  // Parse a single assignment.  Called by ParseAssignDecl()
  absl::Status ParseOneAssignment(Module* module, absl::string_view lhs_name,
                                  absl::optional<Range> lhs_range);

  // Attempts to parse a range of the kind [high:low].  It also handles
  // indexing by setting parameter strict to false, by representing the range as
  // [high:high].  For example:
  //   "a" --> no range
  //   "a[1] --> [1:1] (strict == false)
  //   "a[1:0] --> [1:0]
  absl::StatusOr<absl::optional<Range>> ParseOptionalRange(bool strict = true);

  // Parses a module-level statement (e.g. wire decl or cell instantiation).
  absl::Status ParseModuleStatement(Module* module, Netlist& netlist);

  // Parses a module definition (e.g. at the top of the file).
  absl::StatusOr<std::unique_ptr<Module>> ParseModule(Netlist& netlist);

  // Parses a reference to an already- declared net.
  absl::StatusOr<NetRef> ParseNetRef(Module* module);

  // Pops a name token and returns its contents or gives an error status if a
  // name token is not immediately present in the stream.
  absl::StatusOr<std::string> PopNameOrError();

  // Pops a name token and returns its value or gives an error status if a
  // number token is not immediately present in the stream.
  absl::StatusOr<int64_t> PopNumberOrError();

  // Pops either a name or number token or returns an error.
  absl::StatusOr<absl::variant<std::string, int64_t>> PopNameOrNumberOrError();

  // Drops a token of kind target from the head of the stream or gives an error
  // status.
  absl::Status DropTokenOrError(TokenKind target);

  // Drops a keyword token from the head of the stream or gives an error status.
  absl::Status DropKeywordOrError(absl::string_view target);

  // Attempts to drop a token of the target kind, or returns false if that
  // target token kind is not at the head of the token stream.
  bool TryDropToken(TokenKind target);

  // Attempts to drop a keyword token with the value "target" from the head of
  // the token stream, or returns false if it cannot.
  bool TryDropKeyword(absl::string_view target);

  // Pops a parenthesized name list from the token stream and returns it as a
  // vector of those names.
  absl::StatusOr<std::vector<std::string>> PopParenNameList();

  // Cell library definitions are resolved against.
  CellLibrary* cell_library_;

  // Set of (already-parsed) Modules that may be present in the Module currently
  // being processed as Cell-type references.
  absl::flat_hash_map<std::string, Module> modules_;

  // Scanner used for scanning out tokens (in a stream sequence).
  Scanner* scanner_;
};

}  // namespace rtl
}  // namespace netlist
}  // namespace xls

#endif  // XLS_NETLIST_NETLIST_PARSER_H_
