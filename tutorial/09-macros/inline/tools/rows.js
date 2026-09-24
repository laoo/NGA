// A map a person can read, as the bytes a program can draw: the line endings
// come out, and so does the `@` that marks where the level starts. Where it
// was goes back as Constants, which is what a file cannot say for itself.
//
// Two Sections, because the row the program builds each line in is as wide as
// the map and there is no sense in the two knowing that apart.

const START = 0x40;                     // `@`
const FLOOR = 0x2E;                     // `.`

export default ( args, nga ) =>
{
  const map = nga.read( args.map ).filter( b => b !== 10 && b !== 13 );

  if ( map.length % args.width !== 0 )
  {
    nga.error( `${ args.map } is ${ map.length } characters, `
             + `which is no whole number of rows of ${ args.width }` );
  }

  const at = map.indexOf( START );
  if ( at < 0 )
  {
    nga.error( `${ args.map } has no @, so there is nowhere to start` );
  }
  map[ at ] = FLOOR;

  return {
    sections: [
      {
        name: "levelMap",
        in: args.in,
        items: [
          map,
          { constant: "levelWidth", value: args.width },
          { constant: "levelRows", value: map.length / args.width },
          { constant: "levelStartRow", value: Math.floor( at / args.width ) },
          { constant: "levelStartColumn", value: at % args.width }
        ]
      },
      { name: "levelRow", in: args.in, items: [ { reserve: args.width } ] }
    ]
  };
};
