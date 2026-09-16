"""Recommends movies from a follow graph, then edits two graphs in one atomic transaction.

Four collections model the same little social network, each in the modality that suits it: people
and movies as documents, posters as raw binary blobs, and two graphs — who follows whom, and who
watched what.
The recommendation walks those graphs: take everyone the user follows, count the movies they
watched, drop the ones the user has already seen, and rank what is left by how many followees
watched it.

The last two blocks are the point of the example.
Disliking a recommendation has to add a view edge and remove several follow edges together, or the
user could end up unfollowing people while the view that justified it was lost; both land in one
transaction.
Publishing a new movie likewise writes a document and its poster blob as one unit.

Needs `networkx`, `matplotlib`, `requests` and the `ustore` bindings.
Run with `python examples/movie_recommendations.py`.
"""

import json
from collections import Counter

import matplotlib.pyplot as plt
import networkx as nx
import requests
import ustore.smashtable as ustore

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


def flatten(nested):
    """Concatenates an iterable of iterables into one flat list."""
    return [item for inner in nested for item in inner]


def download(link: str) -> bytes:
    """Fetches one poster as raw bytes, which is what a blob collection stores."""
    return requests.get(link, timeout=REQUEST_TIMEOUT_SECONDS).content


def fill(follows, views, posters, people, movies) -> None:
    """Populates all five collections, each through the batch call its modality offers."""
    people.set(tuple(PEOPLE), tuple(json.dumps(person) for person in PEOPLE.values()))
    movie_keys = range(FIRST_MOVIE_KEY, FIRST_MOVIE_KEY + len(MOVIES))
    movies.set(movie_keys, tuple(json.dumps(movie) for movie in MOVIES))
    posters.set(movie_keys, tuple(download(link) for link in POSTER_LINKS))

    for viewer, watched in VIEWS.items():
        for movie in watched:
            views.add_edge(viewer, movie)
    for follower, followee in FOLLOWS:
        follows.add_edge(follower, followee)


def recommend(follows, views, user: int) -> tuple[dict[int, list[int]], list[int]]:
    """Ranks unseen movies by how many of the user's followees watched them.

    Returns each followee's watch list beside the ranked recommendations, since the caller needs
    both to work out who recommended what.
    """
    followees = list(follows.successors(user))
    watched_by_followee = {followee: list(views.successors(followee)) for followee in followees}

    ranking = Counter(flatten(watched_by_followee.values()))
    for already_seen in views.successors(user):
        ranking.pop(already_seen, None)

    recommended = [movie for movie, _ in ranking.most_common(TOP_RECOMMENDATIONS)]
    return watched_by_followee, recommended


def draw(watched_by_followee: dict[int, list[int]], recommended: list[int]) -> None:
    """Draws the followees on one side and the movies they recommend on the other."""
    bipartite = nx.Graph()
    for followee, watched in watched_by_followee.items():
        for movie in watched:
            if movie in recommended:
                bipartite.add_edge(followee, movie)

    followees = [node for node in bipartite if node in watched_by_followee]
    nx.draw_networkx(bipartite, pos=nx.bipartite_layout(bipartite, followees))
    plt.show()


def dislike(database, user: int, movie: int, recommenders: list[int]) -> None:
    """Records the view and drops every follow edge that led to it, as one atomic transaction."""
    with ustore.Transaction(database) as transaction:
        transaction[VIEWS_COLLECTION].graph.add_edge(user, movie)
        transaction[FOLLOWS_COLLECTION].graph.remove_edges_from([user] * len(recommenders), recommenders)


def publish(database) -> None:
    """Writes a new movie's document and its poster together, so neither lands without the other."""
    with ustore.Transaction(database) as transaction:
        transaction[MOVIES_COLLECTION].docs[NEW_MOVIE_KEY] = NEW_MOVIE
        transaction[POSTERS_COLLECTION][NEW_MOVIE_KEY] = download(NEW_MOVIE_POSTER_LINK)


def main() -> None:
    """Fills the collections, recommends movies to David, draws them and then edits both graphs."""
    database = ustore.DataBase()
    follows = database[FOLLOWS_COLLECTION].graph
    views = database[VIEWS_COLLECTION].graph
    posters = database[POSTERS_COLLECTION]
    people = database[PEOPLE_COLLECTION].docs
    movies = database[MOVIES_COLLECTION].docs
    fill(follows, views, posters, people, movies)

    user = DAVID
    watched_by_followee, recommended = recommend(follows, views, user)
    print(f"Recommending {recommended} to {PEOPLE[user]['name']}")
    draw(watched_by_followee, recommended)

    # The user watches the top pick, dislikes it, and unfollows everyone whose viewing suggested it.
    disliked = recommended[0]
    recommenders = [followee for followee, watched in watched_by_followee.items() if disliked in watched]
    dislike(database, user, disliked, recommenders)
    publish(database)


if __name__ == "__main__":
    main()
