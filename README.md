# cockroachdb/rocksdb

This repository is [CockroachDB]'s fork of [RocksDB], a persistent key-value
store for fast storage.

It exists for two reasons: to provide a home for CockroachDB-specific patches,
as necessary, and to ensure that RocksDB submodule in
[cockroachdb/cockroach][cockroachdb] has a company-controlled remote to point
at.

It is critical that any commit in this repository that is referenced from
[cockroachdb/cockroach][cockroachdb] remains available here in perpetuity. For
every referenced commit, there must be at least one named branch or tag here
that has the commit as an ancestor, or else the commit will be garbage collected
by GitHub.

**To add a patch to an existing release branch:**

  1. In your [cockroachdb/cockroach][cockroachdb] clone, change into the
     submodule's directory, `c-deps/rocksdb`:

     ```shell
     $ cd $GOPATH/src/github.com/cockroachdb/cockroach
     $ cd c-deps/rocksdb
     ```

  2. Create a feature branch named after yourself, like `pmattis/range-del`.

  3. Commit your patch.

  4. Push your changes to the named feature branch. External contributors
     will need to fork this repository and push to their fork instead.

  5. Open a pull request on `cockroachdb/rocksdb` to merge your feature branch
     into the branch that `c-deps/rocksdb` on `cockroachdb/cockroach` is
     pointing to. The PR diff should just be your changes. If you see more than
     just your changes in the diff, you're doing something wrong or these
     instructions are out of date.

  6. Once your `cockroachdb/rocksdb` PR has been approved, merge it into the
     release branch  by pressing the green Merge button, or ask someone with
     commit access to do so.

  7. Make a commit in [cockroachdb/cockroach][cockroachdb] that updates the
     submodule ref to point to the latest SHA on the `cockroachdb/rocksdb`
     release branch you merged into.

     ```shell
     $ cd $GOPATH/src/github.com/cockroachdb/cockroach
     $ cd c-deps/rocksdb
     $ git fetch --all
     $ git checkout <SHA>
     $ cd ..
     $ git add c-deps/rocksdb
     $ git commit -m "c-deps/rocksdb: upgrade to..."
     ```

  8. Open a pull request against [cockroachdb/cockroach][cockroachdb] and wait
     for review. Check Github's diff view to see what changes you're pulling in
     from the `cockroachdb/rocksdb` repository; it should just be the changes
     since the last reference bump, and ideally just your changes.

  9. Once your `cockroachdb/cockroach` PR has been reviewed and LGTM'd, follow
     the usual `cockroachdb/cockroach` merge procedure through bors, or request
     someone with commit access to do so.

  10. From the GitHub web UI, verify that the exact submodule SHA that landed
     in [cockroachdb/cockroach][cockroachdb] is on the appropriate release
     branch. If it is, delete your feature branch in `cockroachdb/rocksdb`.

     ```shell
     $ git push -d origin FEATURE-BRANCH
     ```

**To create a new release branch:**

  1. Follow step one above.

  2. Add an upstream remote and fetch from it.

     ```shell
     $ git remote add upstream https://github.com/facebook/rocksdb
     $ git fetch upstream
     ```

  3. Create and push a new branch with the desired start point:

     ```shell
     $ git checkout -b crl-release-X.X upstream/RELEASE-BRANCH
     $ git push origin crl-release-X.X
     ```

  4. Protect the branch from force-pushes in the repository settings. This is
     crucial in ensuring that we don't break commit references in
     [cockroachdb/cockroach][cockroachdb]'s submodules.

  5. Follow steps seven through nine above.

[CockroachDB]: https://github.com/cockroachdb/cockroach
[RocksDB]: https://github.com/facebook/rocksdb 
