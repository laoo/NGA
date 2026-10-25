// Names are held as clang-tidy holds them in the C++, by the rules of CLAUDE.md:
// types `CamelCase`, functions and variables `camelBack`, a private or protected
// member `mMember`, and a constant `UPPER_CASE` at module scope and `camelBack`
// inside a function. A property is named by whatever format it mirrors — the
// protocol's, the facts' — and is left alone. Nothing else is linted: the style
// is dprint's, and the rest is what `tsc --strict` already refuses.
import tseslint from "typescript-eslint";

export default tseslint.config(
  { ignores: [ "dist/**", "node_modules/**", "eslint.config.mjs" ] },
  {
    files: [ "src/**/*.ts", "test/**/*.ts" ],
    languageOptions: {
      parser: tseslint.parser,
      parserOptions: { projectService: true, tsconfigRootDir: import.meta.dirname }
    },
    plugins: { "@typescript-eslint": tseslint.plugin },
    rules: {
      "@typescript-eslint/naming-convention": [
        "error",
        { selector: "default", format: [ "camelCase" ] },
        { selector: "typeLike", format: [ "PascalCase" ] },
        { selector: "import", format: [ "camelCase", "PascalCase" ] },
        { selector: "classProperty", modifiers: [ "private" ], format: [ "PascalCase" ], prefix: [ "m" ] },
        { selector: "classProperty", modifiers: [ "protected" ], format: [ "PascalCase" ], prefix: [ "m" ] },
        // A table of constants is a constant, and the connection a module holds
        // is not, so an object at module scope may be either; a number, a
        // string or a flag there is always a constant.
        { selector: "variable", modifiers: [ "const", "global" ], format: [ "UPPER_CASE", "camelCase" ] },
        {
          selector: "variable",
          modifiers: [ "const", "global" ],
          types: [ "boolean", "string", "number" ],
          format: [ "UPPER_CASE" ]
        },
        { selector: [ "objectLiteralProperty", "typeProperty" ], format: null }
      ]
    }
  }
);
