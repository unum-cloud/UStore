"""Recommends movies from a follow graph, then edits two graphs in one atomic transaction.

Five collections model the same little social network, each in the modality that suits it: people
and movies as documents, posters as raw binary blobs, and two graphs — who follows whom, and who
watched what.
The recommendation walks those graphs: take everyone the user follows, count the movies they
watched, drop the ones the user has already seen, and rank what is left by how many followees
watched it.

The last two blocks are the point of the example.
Disliking a recommendation has to add a view edge and remove several follow edges together, or the
user could end up unfollowing people while the view that justified it was lost; both land in one
transaction over the same two graph collections.
Publishing a new movie likewise writes a document and its poster blob as one unit.

Needs `matplotlib`, `requests` and the `ustore` bindings.
Run with `python examples/movie_recommendations.py`.

File: examples/movie_recommendations.py
Author: Ash Vardanian
Date: September 16, 2026
"""

import json
from collections import Counter
from collections.abc import Iterable
from typing import NamedTuple

import matplotlib.pyplot as plt
import requests

import ustore
from ustore.graph_view import DiGraph

FOLLOWS_COLLECTION = "people->people"
VIEWS_COLLECTION = "people->movies"
POSTERS_COLLECTION = "movies.poster"
PEOPLE_COLLECTION = "people"
MOVIES_COLLECTION = "movies"

ALICE, BOB, CHARLIE, DAVID = 1, 2, 3, 4
FIRST_MOVIE_KEY = 101
NEW_MOVIE_KEY = 100
TOP_RECOMMENDATIONS = 10
REQUEST_TIMEOUT_SECONDS = 30
USER_AGENT = "ustore-examples/1.0 (https://github.com/unum-cloud/UStore)"

PEOPLE = {
    ALICE: {"name": "Alice", "lastname": "Bobchinsky", "stars": 1200},
    BOB: {"name": "Bob", "lastname": "Charleston", "stars": 700},
    CHARLIE: {"name": "Charlie", "lastname": "Allison", "stars": 800},
    DAVID: {"name": "David", "lastname": "Davidson", "stars": 500},
}

MOVIES = [
    {"title": "The Fast and the Furious", "rating": 6.8},
    {"title": "2 Fast 2 Furious", "rating": 5.9},
    {"title": "The Fast and the Furious: Tokyo Drift", "rating": 6.0},
    {"title": "Fast & Furious", "rating": 6.5},
    {"title": "Fast Five", "rating": 7.3},
    {"title": "Fast & Furious 6", "rating": 7.0},
    {"title": "Furious 7", "rating": 7.1},
    {"title": "The Fate of the Furious", "rating": 6.6},
    {"title": "F9", "rating": 5.2},
]

POSTER_LINKS = [
    "https://upload.wikimedia.org/wikipedia/en/5/54/Fast_and_the_furious_poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/9/9d/Two_fast_two_furious_ver5.jpg",
    "https://upload.wikimedia.org/wikipedia/en/4/4f/Poster_-_Fast_and_Furious_Tokyo_Drift.jpg",
    "https://upload.wikimedia.org/wikipedia/en/8/8f/Fast_and_Furious_Poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/0/0c/Fast_Five_poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/3/30/Fast_%26_Furious_6_film_poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/b/b8/Furious_7_poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/2/2d/The_Fate_of_The_Furious_Theatrical_Poster.jpg",
    "https://upload.wikimedia.org/wikipedia/en/2/2b/F9_film_poster.jpg",
]

NEW_MOVIE = {
    "title": "Fast & Extremely Furious 100",
    "text": "Dominic is racing on rockets in space.",
    "rating": 5,
}
NEW_MOVIE_POSTER_LINK = POSTER_LINKS[0]

# Alice watches the even entries, Bob the middle of the series, Charlie the first five, and David
# nothing at all, so David is the one with recommendations to make.
VIEWS = {
    ALICE: [102, 104, 106, 108],
    BOB: [103, 104, 105, 107],
    CHARLIE: [101, 102, 103, 104, 105],
}
FOLLOWS = [(ALICE, BOB), (DAVID, ALICE), (DAVID, BOB), (DAVID, CHARLIE)]


class Store(NamedTuple):
    """The open database beside the two graph views and the three collections keyed by movie or person."""

    database: ustore.Database
    follows: DiGraph
    views: DiGraph
    people: int
    movies: int
    posters: int


def flatten[Item](nested: Iterable[Iterable[Item]]) -> list[Item]:
    """Concatenates an iterable of iterables into one flat list."""
    return [item for inner in nested for item in inner]


def download(link: str) -> bytes:
    """Fetches one poster as raw bytes, which is what a blob collection stores."""
    response = requests.get(link, timeout=REQUEST_TIMEOUT_SECONDS, headers={"User-Agent": USER_AGENT})
    response.raise_for_status()
    return response.content


def open_store(database: ustore.Database) -> Store:
    """Creates the five collections and wraps the two graphs in NetworkX-compatible views."""
    return Store(
        database=database,
        follows=DiGraph(database, FOLLOWS_COLLECTION),
        views=DiGraph(database, VIEWS_COLLECTION),
        people=database.collection_create(PEOPLE_COLLECTION, modality=ustore.Modality.DOCS),
        movies=database.collection_create(MOVIES_COLLECTION, modality=ustore.Modality.DOCS),
        posters=database.collection_create(POSTERS_COLLECTION, modality=ustore.Modality.BLOBS),
    )


def fill(store: Store) -> None:
    """Populates all five collections, each through the batch call its modality offers."""
    people = [json.dumps(person) for person in PEOPLE.values()]
    store.database.docs_write(list(PEOPLE), people, collection=store.people)

    movie_keys = list(range(FIRST_MOVIE_KEY, FIRST_MOVIE_KEY + len(MOVIES)))
    store.database.docs_write(movie_keys, [json.dumps(movie) for movie in MOVIES], collection=store.movies)
    store.database.write(movie_keys, [download(link) for link in POSTER_LINKS], collection=store.posters)

    viewers = flatten([viewer] * len(watched) for viewer, watched in VIEWS.items())
    store.views.add_edges_from_arrays(viewers, flatten(VIEWS.values()))
    store.follows.add_edges_from_arrays([follower for follower, _ in FOLLOWS], [followee for _, followee in FOLLOWS])


def recommend(store: Store, user: int) -> tuple[dict[int, list[int]], list[int]]:
    """Ranks unseen movies by how many of the user's followees watched them.

    Returns each followee's watch list beside the ranked recommendations, since the caller needs
    both to work out who recommended what.
    """
    followees = list(store.follows.successors(user))
    watched_by_followee = {followee: list(store.views.successors(followee)) for followee in followees}

    ranking = Counter(flatten(watched_by_followee.values()))
    for already_seen in store.views.successors(user):
        ranking.pop(already_seen, None)

    recommended = [movie for movie, _ in ranking.most_common(TOP_RECOMMENDATIONS)]
    return watched_by_followee, recommended


def titles(store: Store, movies: list[int]) -> list[str]:
    """The title of every movie, read as one batch out of the documents collection."""
    documents = store.database.docs_read(
        movies, ustore.Arena(), field="title", outputs_type=ustore.FieldType.STRING, collection=store.movies
    )
    return ["" if document is None else bytes(document).decode() for document in documents]


def draw(store: Store, watched_by_followee: dict[int, list[int]], recommended: list[int]) -> None:
    """Draws the followees on one side and the movies they recommend on the other."""
    followees = sorted(watched_by_followee)
    labelled = dict(zip(recommended, titles(store, recommended), strict=True))
    left = {followee: index for index, followee in enumerate(followees)}
    right = {movie: index for index, movie in enumerate(recommended)}

    figure, axes = plt.subplots()
    for followee, watched in watched_by_followee.items():
        for movie in watched:
            if movie in right:
                axes.plot([0, 1], [left[followee], right[movie]], color="0.7", zorder=1)
    axes.scatter([0] * len(left), list(left.values()), color="tab:blue", zorder=2)
    axes.scatter([1] * len(right), list(right.values()), color="tab:orange", zorder=2)
    for followee, row in left.items():
        axes.annotate(PEOPLE[followee]["name"], (0, row), horizontalalignment="right")
    for movie, row in right.items():
        axes.annotate(labelled[movie], (1, row), horizontalalignment="left")

    axes.set_axis_off()
    axes.set_xlim(-1, 2)
    figure.tight_layout()
    plt.show()


def dislike(store: Store, user: int, movie: int, recommenders: list[int]) -> None:
    """Records the view and drops every follow edge that led to it, as one atomic transaction."""
    with store.database.transaction() as transaction:
        DiGraph(transaction, store.views.graph_collection).add_edge(user, movie)
        DiGraph(transaction, store.follows.graph_collection).remove_edges_from_arrays(
            [user] * len(recommenders), recommenders
        )


def publish(store: Store) -> None:
    """Writes a new movie's document and its poster together, so neither lands without the other."""
    poster = download(NEW_MOVIE_POSTER_LINK)
    with store.database.transaction() as transaction:
        transaction.docs_write([NEW_MOVIE_KEY], json.dumps(NEW_MOVIE), collection=store.movies)
        transaction.set(NEW_MOVIE_KEY, poster, collection=store.posters)


def main() -> None:
    """Fills the collections, recommends movies to David, draws them and then edits both graphs."""
    with ustore.Database.open_ram() as database:
        store = open_store(database)
        fill(store)

        user = DAVID
        watched_by_followee, recommended = recommend(store, user)
        print(f"Recommending {titles(store, recommended)} to {PEOPLE[user]['name']}")
        draw(store, watched_by_followee, recommended)

        # The user watches the top pick, dislikes it, and unfollows everyone whose viewing suggested it.
        disliked = recommended[0]
        recommenders = [followee for followee, watched in watched_by_followee.items() if disliked in watched]
        dislike(store, user, disliked, recommenders)
        print(f"{PEOPLE[user]['name']} now follows {sorted(store.follows.successors(user))}")

        publish(store)
        published = store.database.get(NEW_MOVIE_KEY, collection=store.posters)
        print(f"Published {NEW_MOVIE['title']} with a {len(published)} byte poster")


if __name__ == "__main__":
    main()
