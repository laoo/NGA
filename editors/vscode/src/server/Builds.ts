import type { Diagnostic } from "vscode-languageserver/node";

import type { FactsDocument } from "./Facts.ts";
import { findingsOf } from "./Findings.ts";
import type { TextOf } from "./Findings.ts";
import { RunCancelled } from "./Nga.ts";
import type { Run } from "./Nga.ts";

// How a Project is built for its findings.
export type Builder = ( project: string ) => Run;

// What the server does with the findings shown at a URI: the whole of them,
// each time, which is how the protocol replaces what a document showed before.
export type Publisher = ( uri: string, diagnostics: Diagnostic[] ) => void;

// The builds of every Project the reader has opened a file of, and what each
// last said about each file.
//
// A newer build of a Project cancels the older one rather than waiting behind
// it: two saves in a second are one build. What a build says about a document
// replaces what the last build of the same Project said, and a document it
// says nothing about any more is cleared. A file two Projects build — one of
// the shipped library, say — shows what both last said.
export class Builds
{
  private mRuns = new Map<string, Run>();
  private mShown = new Map<string, Map<string, Diagnostic[]>>();
  private mBuild: Builder;
  private mPublish: Publisher;
  private mTextOf: TextOf;
  private mFailed: ( project: string, error: unknown ) => void;
  private mFacts = new Map<string, FactsDocument>();
  private mBuilt: ( project: string ) => void;

  constructor(
    build: Builder,
    publish: Publisher,
    textOf: TextOf,
    failed: ( project: string, error: unknown ) => void,
    built: ( project: string ) => void
  )
  {
    this.mBuild = build;
    this.mPublish = publish;
    this.mTextOf = textOf;
    this.mFailed = failed;
    this.mBuilt = built;
  }

  // What the last build of a Project that finished said, failed or not: the
  // names it knew are as good as its findings.
  factsOf( project: string ): FactsDocument | undefined
  {
    return this.mFacts.get( project );
  }

  // Every Project built so far, with what its last build said.
  everyBuild(): [ string, FactsDocument ][]
  {
    return [ ...this.mFacts ];
  }

  // Whether the Project has been built, or is being built, since the server
  // started: a file opened of a Project already known needs no build of its own.
  isKnown( project: string ): boolean
  {
    return this.mShown.has( project ) || this.mRuns.has( project );
  }

  async build( project: string ): Promise<void>
  {
    this.mRuns.get( project )?.cancel();
    const run = this.mBuild( project );
    this.mRuns.set( project, run );
    let found: Map<string, Diagnostic[]>;
    try
    {
      const facts = await run.facts;
      found = findingsOf( facts, project, this.mTextOf );
      this.mFacts.set( project, facts );
    }
    catch ( error )
    {
      if ( !( error instanceof RunCancelled ) )
      {
        this.mFailed( project, error );
      }
      return;
    }
    finally
    {
      if ( this.mRuns.get( project ) === run )
      {
        this.mRuns.delete( project );
      }
    }

    const before = this.mShown.get( project ) ?? new Map<string, Diagnostic[]>();
    this.mShown.set( project, found );
    for ( const uri of new Set( [ ...before.keys(), ...found.keys() ] ) )
    {
      this.mPublish( uri, [ ...this.mShown.values() ].flatMap( ( shown ) => shown.get( uri ) ?? [] ) );
    }
    this.mBuilt( project );
  }
}
