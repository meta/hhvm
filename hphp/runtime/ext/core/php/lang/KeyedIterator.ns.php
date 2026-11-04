<?hh

namespace HH {

interface KeyedIterator<+Tk, +Tv> extends Iterator<Tv>, KeyedTraversable<Tk, Tv> {
}

}
