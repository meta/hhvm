<?hh

enum YES: string as string {
  YES = nameof self;
}

enum YEAH: string as string {
  use YES;
  YEAH = nameof self;
}
