(*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the "hack" directory of this source tree.
 *)

(**
 * Generate a compact outline of a Hack file with line range annotations,
 * for AI agents. Helpful for determining which ranges of the file to read.
 *)

open Hh_prelude
module Syn = Full_fidelity_positioned_syntax
module Syntax_tree = Full_fidelity_syntax_tree.WithSyntax (Syn)

(** Include preceding doc comments, but not regular comments *)
let line_range (src : Full_fidelity_source_text.t) (node : Syn.t) : int * int =
  let trivia_list = Syn.leading_trivia node in
  let doc_comment_offset =
    List.fold trivia_list ~init:None ~f:(fun acc t ->
        if
          Full_fidelity_trivia_kind.equal
            (Full_fidelity_positioned_trivia.kind t)
            Full_fidelity_trivia_kind.DelimitedComment
          && String.is_prefix
               (Full_fidelity_positioned_trivia.text t)
               ~prefix:"/**"
        then
          Some (Full_fidelity_positioned_trivia.start_offset t)
        else
          acc)
  in
  let start_offset =
    match doc_comment_offset with
    | Some off -> off
    | None -> Syn.start_offset node
  in
  let start_line =
    fst (Full_fidelity_source_text.offset_to_position src start_offset)
  in
  let end_line =
    fst (Full_fidelity_source_text.offset_to_position src (Syn.end_offset node))
  in
  (start_line, end_line)

(** Source text of [node] excluding leading/trailing trivia. *)
let node_text (src : Full_fidelity_source_text.t) (node : Syn.t) : string =
  Full_fidelity_source_text.sub src (Syn.start_offset node) (Syn.width node)

let to_line_range_annotation ((start_line, end_line) : int * int) : string =
  if start_line = end_line then
    Printf.sprintf " | line %d" start_line
  else
    Printf.sprintf " | lines %d-%d" start_line end_line

let has_modifier (is_kind : Syn.t -> bool) (modifiers : Syn.t) : bool =
  List.exists (Syn.syntax_node_to_list modifiers) ~f:is_kind

(** Extract just the names from a list of declarators, stripping initializers. *)
let declarator_names (src : Full_fidelity_source_text.t) (node : Syn.t) : string
    =
  let name_of_declarator (d : Syn.t) : string =
    match Syn.syntax d with
    | ConstantDeclarator { constant_declarator_name; _ } ->
      String.strip (node_text src constant_declarator_name)
    | PropertyDeclarator { property_name; _ } ->
      String.strip (node_text src property_name)
    | _ -> String.strip (node_text src d)
  in
  match Syn.syntax node with
  | SyntaxList items ->
    List.filter_map items ~f:(fun item ->
        match Syn.syntax item with
        | ListItem { list_item; _ } -> Some (name_of_declarator list_item)
        | _ -> Some (name_of_declarator item))
    |> String.concat ~sep:", "
  | _ -> String.strip (node_text src node)

let write_member
    (buf : Buffer.t)
    (indent : string)
    (src : Full_fidelity_source_text.t)
    (node : Syn.t) : unit =
  let suffix = to_line_range_annotation (line_range src node) in
  match Syn.syntax node with
  | Syn.MethodishDeclaration { methodish_function_decl_header; _ } ->
    (match Syn.syntax methodish_function_decl_header with
    | FunctionDeclarationHeader { function_modifiers; function_name; _ } ->
      if not (has_modifier Syn.is_private function_modifiers) then
        Printf.bprintf
          buf
          "%sfunction %s%s\n"
          indent
          (String.strip (node_text src function_name))
          suffix
    | _ -> ())
  | PropertyDeclaration { property_modifiers; property_declarators; _ } ->
    if not (has_modifier Syn.is_private property_modifiers) then
      Printf.bprintf
        buf
        "%s%s%s\n"
        indent
        (declarator_names src property_declarators)
        suffix
  | ConstDeclaration { const_modifiers; const_declarators; _ } ->
    if not (has_modifier Syn.is_private const_modifiers) then
      Printf.bprintf
        buf
        "%sconst %s%s\n"
        indent
        (declarator_names src const_declarators)
        suffix
  | TypeConstDeclaration { type_const_modifiers; type_const_name; _ } ->
    if not (has_modifier Syn.is_private type_const_modifiers) then
      Printf.bprintf
        buf
        "%sconst type %s%s\n"
        indent
        (String.strip (node_text src type_const_name))
        suffix
  | TraitUse _
  | RequireClause _
  | MethodishTraitResolution _
  | ContextConstDeclaration _
  | RequireClauseConstraint _
  | XHPClassAttributeDeclaration _
  | XHPChildrenDeclaration _
  | XHPCategoryDeclaration _ -> ()
  (* Not class body declarations — exhaustive so the compiler catches new constructors *)
  | Token _ | Missing | SyntaxList _ | EndOfFile _ | ErrorSyntax _ | Script _ | QualifiedName _ | ModuleName _ | SimpleTypeSpecifier _ | LiteralExpression _ | PrefixedStringExpression _ | PrefixedCodeExpression _ | VariableExpression _ | PipeVariableExpression _ | FileAttributeSpecification _ | EnumDeclaration _ | EnumUse _ | Enumerator _ | EnumClassDeclaration _ | EnumClassEnumerator _ | AliasDeclaration _ | ContextAliasDeclaration _ | CaseTypeDeclaration _ | CaseTypeVariant _ | PropertyDeclarator _ | NamespaceDeclaration _ | NamespaceDeclarationHeader _ | NamespaceBody _ | NamespaceEmptyBody _ | NamespaceUseDeclaration _ | NamespaceGroupUseDeclaration _ | NamespaceUseClause _ | FunctionDeclaration _ | FunctionDeclarationHeader _ | Contexts _ | WhereClause _ | WhereConstraint _ | ClassishDeclaration _ | ClassishBody _ | ClassAliasDeclaration _ | ConstantDeclarator _ | DecoratedExpression _ | NamedArgument _ | ParameterDeclaration _ | OldAttributeSpecification _ | InclusionExpression _ | InclusionDirective _ | CompoundStatement _ | ExpressionStatement _ | MarkupSection _ | MarkupSuffix _ | UnsetStatement _ | UsingStatementBlockScoped _ | UsingStatementFunctionScoped _ | WhileStatement _ | IfStatement _ | ElseClause _ | TryStatement _ | CatchClause _ | FinallyClause _ | DoStatement _ | ForStatement _ | ForeachStatement _ | SwitchStatement _ | SwitchSection _ | SwitchFallthrough _ | CaseLabel _ | DefaultLabel _ | MatchStatement _ | MatchStatementArm _ | ReturnStatement _ | YieldBreakStatement _ | ThrowStatement _ | BreakStatement _ | ContinueStatement _ | EchoStatement _ | ConcurrentStatement _ | SimpleInitializer _ | AnonymousClass _ | AnonymousFunction _ | AnonymousFunctionUseClause _ | LambdaExpression _ | LambdaSignature _ | CastExpression _ | ScopeResolutionExpression _ | MemberSelectionExpression _ | SafeMemberSelectionExpression _ | EmbeddedMemberSelectionExpression _ | YieldExpression _ | PrefixUnaryExpression _ | PostfixUnaryExpression _ | BinaryExpression _ | IsExpression _ | AsExpression _ | NullableAsExpression _ | UpcastExpression _ | ConditionalExpression _ | EvalExpression _ | IssetExpression _ | NameofExpression _ | FunctionCallExpression _ | FunctionPointerExpression _ | ParenthesizedExpression _ | BracedExpression _ | ETSpliceExpression _ | EmbeddedBracedExpression _ | ListExpression _ | CollectionLiteralExpression _ | ObjectCreationExpression _ | ConstructorCall _ | DarrayIntrinsicExpression _ | DictionaryIntrinsicExpression _ | KeysetIntrinsicExpression _ | VarrayIntrinsicExpression _ | VectorIntrinsicExpression _ | ElementInitializer _ | SubscriptExpression _ | EmbeddedSubscriptExpression _ | AwaitableCreationExpression _ | XHPChildrenParenthesizedList _ | XHPEnumType _ | XHPLateinit _ | XHPRequired _ | XHPClassAttribute _ | XHPSimpleClassAttribute _ | XHPSimpleAttribute _ | XHPSpreadAttribute _ | XHPOpen _ | XHPExpression _ | XHPClose _ | TypeConstant _ | VectorTypeSpecifier _ | KeysetTypeSpecifier _ | TupleTypeExplicitSpecifier _ | VarrayTypeSpecifier _ | FunctionCtxTypeSpecifier _ | TypeParameter _ | TypeConstraint _ | ContextConstraint _ | DarrayTypeSpecifier _ | DictionaryTypeSpecifier _ | ClosureTypeSpecifier _ | ClosureParameterTypeSpecifier _ | TypeRefinement _ | TypeInRefinement _ | CtxInRefinement _ | ClassnameTypeSpecifier _ | ClassPtrTypeSpecifier _ | FieldSpecifier _ | AbsentFieldSpecifier _ | FieldInitializer _ | ShapeTypeSpecifier _ | ShapeSplatExpression _ | ShapeExpression _ | TupleExpression _ | GenericTypeSpecifier _ | NullableTypeSpecifier _ | LikeTypeSpecifier _ | AttributizedSpecifier _ | ReifiedTypeArgument _ | TypeArguments _ | TypeParameters _ | TupleTypeSpecifier _ | UnionTypeSpecifier _ | IntersectionTypeSpecifier _ | TupleOrUnionOrIntersectionElementTypeSpecifier _ | ListItem _ | EnumClassLabelExpression _ | ModuleDeclaration _ | ModuleMembershipDeclaration _ | PackageExpression _ | ConstructorPattern _ | RefinementPattern _ | VariablePattern _ | ShapeSplatSpecifier _
    -> ()
[@@ocamlformat "disable"]

let write_members
    (buf : Buffer.t)
    (indent : string)
    (src : Full_fidelity_source_text.t)
    (elements_node : Syn.t) : unit =
  match Syn.syntax elements_node with
  | SyntaxList items ->
    List.iter items ~f:(fun item -> write_member buf indent src item)
  | _ -> ()

let rec write_top_level
    (buf : Buffer.t)
    (indent : string)
    (src : Full_fidelity_source_text.t)
    (node : Syn.t) : unit =
  let suffix = to_line_range_annotation (line_range src node) in
  match Syn.syntax node with
  | Syn.FunctionDeclaration { function_declaration_header; _ } ->
    (match Syn.syntax function_declaration_header with
    | FunctionDeclarationHeader { function_name; _ } ->
      Printf.bprintf
        buf
        "%sfunction %s%s\n"
        indent
        (String.strip (node_text src function_name))
        suffix
    | _ -> ())
  | ClassishDeclaration
      { classish_modifiers; classish_keyword; classish_name; classish_body; _ }
    ->
    let kw = String.strip (node_text src classish_keyword) in
    let kind =
      if has_modifier Syn.is_abstract classish_modifiers then
        "abstract " ^ kw
      else
        kw
    in
    let name = String.strip (node_text src classish_name) in
    Printf.bprintf buf "%s%s %s%s\n" indent kind name suffix;
    let member_indent = indent ^ "  " in
    (match Syn.syntax classish_body with
    | ClassishBody { classish_body_elements; _ } ->
      write_members buf member_indent src classish_body_elements
    | _ -> ())
  | EnumDeclaration { enum_name; enum_enumerators; _ } ->
    let name = String.strip (node_text src enum_name) in
    Printf.bprintf buf "%senum %s%s\n" indent name suffix;
    let member_indent = indent ^ "  " in
    (match Syn.syntax enum_enumerators with
    | SyntaxList items ->
      List.iter items ~f:(fun item ->
          match Syn.syntax item with
          | Enumerator { enumerator_name; _ } ->
            Printf.bprintf
              buf
              "%s%s\n"
              member_indent
              (String.strip (node_text src enumerator_name))
          | _ -> ())
    | _ -> ())
  | EnumClassDeclaration { enum_class_name; enum_class_elements; _ } ->
    let name = String.strip (node_text src enum_class_name) in
    Printf.bprintf buf "%senum class %s%s\n" indent name suffix;
    let member_indent = indent ^ "  " in
    (match Syn.syntax enum_class_elements with
    | SyntaxList items ->
      List.iter items ~f:(fun item ->
          match Syn.syntax item with
          | EnumClassEnumerator { enum_class_enumerator_name; _ } ->
            Printf.bprintf
              buf
              "%s%s\n"
              member_indent
              (String.strip (node_text src enum_class_enumerator_name))
          | _ -> ())
    | _ -> ())
  | AliasDeclaration { alias_keyword; alias_name; _ } ->
    let kw = String.strip (node_text src alias_keyword) in
    let name = String.strip (node_text src alias_name) in
    Printf.bprintf buf "%s%s %s%s\n" indent kw name suffix
  | ConstDeclaration { const_declarators; _ } ->
    let names = declarator_names src const_declarators in
    Printf.bprintf buf "%sconst %s%s\n" indent names suffix
  | NamespaceDeclaration { namespace_header; namespace_body } ->
    let header_text = String.strip (node_text src namespace_header) in
    Printf.bprintf buf "%s%s%s\n" indent header_text suffix;
    (match Syn.syntax namespace_body with
    | NamespaceBody { namespace_declarations; _ } ->
      let body_indent = indent ^ "  " in
      (match Syn.syntax namespace_declarations with
      | SyntaxList items ->
        List.iter items ~f:(fun item ->
            write_top_level buf body_indent src item)
      | _ -> ())
    | _ -> ())
  | ModuleDeclaration { module_declaration_name; _ } ->
    let name = String.strip (node_text src module_declaration_name) in
    Printf.bprintf buf "%smodule %s%s\n" indent name suffix
  | ModuleMembershipDeclaration { module_membership_declaration_name; _ } ->
    let name = String.strip (node_text src module_membership_declaration_name) in
    Printf.bprintf buf "%smodule %s%s\n" indent name suffix
  | CaseTypeDeclaration { case_type_name; _ } ->
    let name = String.strip (node_text src case_type_name) in
    Printf.bprintf buf "%scase type %s%s\n" indent name suffix
  | ContextAliasDeclaration { ctx_alias_name; _ } ->
    let name = String.strip (node_text src ctx_alias_name) in
    Printf.bprintf buf "%snewctx %s%s\n" indent name suffix
  | ClassAliasDeclaration { class_alias_keyword; class_alias_name; class_alias_original_name; _ } ->
    let kw = String.strip (node_text src class_alias_keyword) in
    let name = String.strip (node_text src class_alias_name) in
    let orig = String.strip (node_text src class_alias_original_name) in
    Printf.bprintf buf "%s%s %s = %s%s\n" indent kw name orig suffix
  (* Not top-level declarations — exhaustive so the compiler catches new constructors *)
  | MarkupSection _ | MarkupSuffix _ | EndOfFile _ | FileAttributeSpecification _ | NamespaceUseDeclaration _ | NamespaceGroupUseDeclaration _ | InclusionDirective _ | ExpressionStatement _ | CompoundStatement _ | Token _ | Missing | SyntaxList _ | ErrorSyntax _ | Script _ | QualifiedName _ | ModuleName _ | SimpleTypeSpecifier _ | LiteralExpression _ | PrefixedStringExpression _ | PrefixedCodeExpression _ | VariableExpression _ | PipeVariableExpression _ | EnumUse _ | Enumerator _ | EnumClassEnumerator _ | CaseTypeVariant _ | PropertyDeclaration _ | PropertyDeclarator _ | NamespaceDeclarationHeader _ | NamespaceBody _ | NamespaceEmptyBody _ | NamespaceUseClause _ | FunctionDeclarationHeader _ | Contexts _ | WhereClause _ | WhereConstraint _ | MethodishDeclaration _ | MethodishTraitResolution _ | ClassishBody _ | TraitUse _ | RequireClause _ | RequireClauseConstraint _ | ConstantDeclarator _ | TypeConstDeclaration _ | ContextConstDeclaration _ | DecoratedExpression _ | NamedArgument _ | ParameterDeclaration _ | OldAttributeSpecification _ | InclusionExpression _ | UnsetStatement _ | UsingStatementBlockScoped _ | UsingStatementFunctionScoped _ | WhileStatement _ | IfStatement _ | ElseClause _ | TryStatement _ | CatchClause _ | FinallyClause _ | DoStatement _ | ForStatement _ | ForeachStatement _ | SwitchStatement _ | SwitchSection _ | SwitchFallthrough _ | CaseLabel _ | DefaultLabel _ | MatchStatement _ | MatchStatementArm _ | ReturnStatement _ | YieldBreakStatement _ | ThrowStatement _ | BreakStatement _ | ContinueStatement _ | EchoStatement _ | ConcurrentStatement _ | SimpleInitializer _ | AnonymousClass _ | AnonymousFunction _ | AnonymousFunctionUseClause _ | LambdaExpression _ | LambdaSignature _ | CastExpression _ | ScopeResolutionExpression _ | MemberSelectionExpression _ | SafeMemberSelectionExpression _ | EmbeddedMemberSelectionExpression _ | YieldExpression _ | PrefixUnaryExpression _ | PostfixUnaryExpression _ | BinaryExpression _ | IsExpression _ | AsExpression _ | NullableAsExpression _ | UpcastExpression _ | ConditionalExpression _ | EvalExpression _ | IssetExpression _ | NameofExpression _ | FunctionCallExpression _ | FunctionPointerExpression _ | ParenthesizedExpression _ | BracedExpression _ | ETSpliceExpression _ | EmbeddedBracedExpression _ | ListExpression _ | CollectionLiteralExpression _ | ObjectCreationExpression _ | ConstructorCall _ | DarrayIntrinsicExpression _ | DictionaryIntrinsicExpression _ | KeysetIntrinsicExpression _ | VarrayIntrinsicExpression _ | VectorIntrinsicExpression _ | ElementInitializer _ | SubscriptExpression _ | EmbeddedSubscriptExpression _ | AwaitableCreationExpression _ | XHPChildrenDeclaration _ | XHPCategoryDeclaration _ | XHPChildrenParenthesizedList _ | XHPEnumType _ | XHPLateinit _ | XHPRequired _ | XHPClassAttribute _ | XHPSimpleClassAttribute _ | XHPClassAttributeDeclaration _ | XHPSimpleAttribute _ | XHPSpreadAttribute _ | XHPOpen _ | XHPExpression _ | XHPClose _ | TypeConstant _ | VectorTypeSpecifier _ | KeysetTypeSpecifier _ | TupleTypeExplicitSpecifier _ | VarrayTypeSpecifier _ | FunctionCtxTypeSpecifier _ | TypeParameter _ | TypeConstraint _ | ContextConstraint _ | DarrayTypeSpecifier _ | DictionaryTypeSpecifier _ | ClosureTypeSpecifier _ | ClosureParameterTypeSpecifier _ | TypeRefinement _ | TypeInRefinement _ | CtxInRefinement _ | ClassnameTypeSpecifier _ | ClassPtrTypeSpecifier _ | FieldSpecifier _ | AbsentFieldSpecifier _ | FieldInitializer _ | ShapeTypeSpecifier _ | ShapeSplatExpression _ | ShapeExpression _ | TupleExpression _ | GenericTypeSpecifier _ | NullableTypeSpecifier _ | LikeTypeSpecifier _ | AttributizedSpecifier _ | ReifiedTypeArgument _ | TypeArguments _ | TypeParameters _ | TupleTypeSpecifier _ | UnionTypeSpecifier _ | IntersectionTypeSpecifier _ | TupleOrUnionOrIntersectionElementTypeSpecifier _ | ListItem _ | EnumClassLabelExpression _ | PackageExpression _ | ConstructorPattern _ | RefinementPattern _ | VariablePattern _ | ShapeSplatSpecifier _
    -> ()
[@@ocamlformat "disable"]

(** Generate a compact outline of a Hack source file with line range annotations. *)
let outline (content : string) : string =
  let src =
    Full_fidelity_source_text.make
      (Relative_path.create Relative_path.Dummy "<outline>")
      content
  in
  let tree = Syntax_tree.make src in
  let root = Syntax_tree.root tree in
  let buf = Buffer.create 4096 (* getconf PAGE_SIZE *) in
  (* Check for parse errors and add warning if present *)
  let errors = Syntax_tree.errors tree in
  if not (List.is_empty errors) then
    Printf.bprintf
      buf
      "WARNING: File contains syntax errors. Outline is best-effort.\n\n";
  (match Syn.syntax root with
  | Syn.Script { script_declarations } ->
    (match Syn.syntax script_declarations with
    | SyntaxList items -> List.iter items ~f:(write_top_level buf "" src)
    | _ -> ())
  | _ -> ());
  Buffer.contents buf
