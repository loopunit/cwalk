#include <assert.h>
#include <ctype.h>
#include <cwalk.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

size_t cwk::get_absolute(const char *base, const char *path, char *buffer,
  size_t buffer_size)
{
  size_t i;
  const char *paths[4];

  // The basename should be an absolute path if the caller is using the API
  // correctly. However, he might not and in that case we will append a fake
  // root at the beginning.
  if (is_absolute(base)) {
    i = 0;
  } else if (path_style == CWK_STYLE_WINDOWS) {
    paths[0] = "\\";
    i = 1;
  } else {
    paths[0] = "/";
    i = 1;
  }

  if (is_absolute(path)) {
    // If the submitted path is not relative the base path becomes irrelevant.
    // We will only normalize the submitted path instead.
    paths[i++] = path;
    paths[i] = NULL;
  } else {
    // Otherwise we append the relative path to the base path and normalize it.
    // The result will be a new absolute path.
    paths[i++] = base;
    paths[i++] = path;
    paths[i] = NULL;
  }

  // Finally join everything together and normalize it.
  return join_and_normalize_multiple(paths, buffer, buffer_size);
}

size_t cwk::get_relative(const char *base_directory, const char *path,
  char *buffer, size_t buffer_size)
{
  size_t pos, base_root_length, path_root_length;
  bool absolute, base_available, other_available, has_output;
  const char *base_paths[2], *other_paths[2];
  struct cwk_segment_joined bsj, osj;

  pos = 0;

  // First we compare the roots of those two paths. If the roots are not equal
  // we can't continue, since there is no way to get a relative path from
  // different roots.
  get_root(base_directory, &base_root_length);
  get_root(path, &path_root_length);
  if (base_root_length != path_root_length ||
      !is_string_equal(base_directory, path, base_root_length)) {
    terminate_output(buffer, buffer_size, pos);
    return pos;
  }

  // Verify whether this is an absolute path. We need to know that since we can
  // remove all back-segments if it is.
  absolute = is_root_absolute(base_directory, base_root_length);

  // Initialize our joined segments. This will allow us to use the internal
  // functions to skip until diverge and invisible. We only have one path in
  // them though.
  base_paths[0] = base_directory;
  base_paths[1] = NULL;
  other_paths[0] = path;
  other_paths[1] = NULL;
  get_first_segment_joined(base_paths, &bsj);
  get_first_segment_joined(other_paths, &osj);

  // Okay, now we skip until the segments diverge. We don't have anything to do
  // with the segments which are equal.
  skip_segments_until_diverge(&bsj, &osj, absolute, &base_available,
    &other_available);

  // Assume there is no output until we have got some. We will need this
  // information later on to remove trailing slashes or alternatively output a
  // current-segment.
  has_output = false;

  // So if we still have some segments left in the base path we will now output
  // a back segment for all of them.
  if (base_available) {
    do {
      // Skip any invisible segment. We don't care about those and we don't need
      // to navigate back because of them.
      if (!segment_joined_skip_invisible(&bsj, absolute)) {
        break;
      }

      // Toggle the flag if we have output. We need to remember that, since we
      // want to remove the trailing slash.
      has_output = true;

      // Output the back segment and a separator. No need to worry about the
      // superfluous segment since it will be removed later on.
      pos += output_back(buffer, buffer_size, pos);
      pos += output_separator(buffer, buffer_size, pos);
    } while (get_next_segment_joined(&bsj));
  }

  // And if we have some segments available of the target path we will output
  // all of those.
  if (other_available) {
    do {
      // Again, skip any invisible segments since we don't need to navigate into
      // them.
      if (!segment_joined_skip_invisible(&osj, absolute)) {
        break;
      }

      // Toggle the flag if we have output. We need to remember that, since we
      // want to remove the trailing slash.
      has_output = true;

      // Output the current segment and a separator. No need to worry about the
      // superfluous segment since it will be removed later on.
      pos += output_sized(buffer, buffer_size, pos, osj.segment.begin,
        osj.segment.size);
      pos += output_separator(buffer, buffer_size, pos);
    } while (get_next_segment_joined(&osj));
  }

  // If we have some output by now we will have to remove the trailing slash. We
  // simply do that by moving back one character. The terminate output function
  // will then place the '\0' on this position. Otherwise, if there is no
  // output, we will have to output a "current directory", since the target path
  // points to the base path.
  if (has_output) {
    --pos;
  } else {
    pos += output_current(buffer, buffer_size, pos);
  }

  // Finally, we can terminate the output - which means we place a '\0' at the
  // current position or at the end of the buffer.
  terminate_output(buffer, buffer_size, pos);

  return pos;
}

size_t cwk::join(const char *path_a, const char *path_b, char *buffer,
  size_t buffer_size)
{
  const char *paths[3];

  // This is simple. We will just create an array with the two paths which we
  // wish to join.
  paths[0] = path_a;
  paths[1] = path_b;
  paths[2] = NULL;

  // And then call the join and normalize function which will do the hard work
  // for us.
  return join_and_normalize_multiple(paths, buffer, buffer_size);
}

size_t cwk::join_multiple(const char **paths, char *buffer,
  size_t buffer_size)
{
  // We can just call the internal join and normalize function for this one,
  // since it will handle everything.
  return join_and_normalize_multiple(paths, buffer, buffer_size);
}

void cwk::get_root(const char *path, size_t *length)
{
  // We use a different implementation here based on the configuration of the
  // library.
  if (path_style == CWK_STYLE_WINDOWS) {
    get_root_windows(path, length);
  } else {
    get_root_unix(path, length);
  }
}

size_t cwk::change_root(const char *path, const char *new_root,
  char *buffer, size_t buffer_size)
{
  const char *tail;
  size_t root_length, path_length, tail_length, new_root_length, new_path_size;

  // First we need to determine the actual size of the root which we will
  // change.
  get_root(path, &root_length);

  // Now we determine the sizes of the new root and the path. We need that to
  // determine the size of the part after the root (the tail).
  new_root_length = strlen(new_root);
  path_length = strlen(path);

  // Okay, now we calculate the position of the tail and the length of it.
  tail = path + root_length;
  tail_length = path_length - root_length;

  // We first output the tail and then the new root, that's because the source
  // path and the buffer may be overlapping. This way the root will not
  // overwrite the tail.
  output_sized(buffer, buffer_size, new_root_length, tail,
    tail_length);
  output_sized(buffer, buffer_size, 0, new_root, new_root_length);

  // Finally we calculate the size o the new path and terminate the output with
  // a '\0'.
  new_path_size = tail_length + new_root_length;
  terminate_output(buffer, buffer_size, new_path_size);

  return new_path_size;
}

bool cwk::is_absolute(const char *path)
{
  size_t length;

  // We grab the root of the path. This root does not include the first
  // separator of a path.
  get_root(path, &length);

  // Now we can determine whether the root is absolute or not.
  return is_root_absolute(path, length);
}

bool cwk::is_relative(const char *path)
{
  // The path is relative if it is not absolute.
  return !is_absolute(path);
}

void cwk::get_basename(const char *path, const char **basename,
  size_t *length)
{
  struct cwk_segment segment;

  // We get the last segment of the path. The last segment will contain the
  // basename if there is any. If there are no segments we will set the basename
  // to NULL and the length to 0.
  if (!get_last_segment(path, &segment)) {
    *basename = NULL;
    *length = 0;
    return;
  }

  // Now we can just output the segment contents, since that's our basename.
  // There might be trailing separators after the basename, but the size does
  // not include those.
  *basename = segment.begin;
  *length = segment.size;
}

size_t cwk::change_basename(const char *path, const char *new_basename,
  char *buffer, size_t buffer_size)
{
  struct cwk_segment segment;
  size_t pos, root_size, new_basename_size;

  // First we try to get the last segment. We may only have a root without any
  // segments, in which case we will create one.
  if (!get_last_segment(path, &segment)) {

    // So there is no segment in this path. First we grab the root and output
    // that. We are not going to modify the root in any way.
    get_root(path, &root_size);
    pos = output_sized(buffer, buffer_size, 0, path, root_size);

    // We have to trim the separators from the beginning of the new basename.
    // This is quite easy to do.
    while (is_separator(new_basename)) {
      ++new_basename;
    }

    // Now we measure the length of the new basename, this is a two step
    // process. First we find the '\0' character at the end of the string.
    new_basename_size = 0;
    while (new_basename[new_basename_size]) {
      ++new_basename_size;
    }

    // And then we trim the separators at the end of the basename until we reach
    // the first valid character.
    while (new_basename_size > 0 &&
           is_separator(&new_basename[new_basename_size - 1])) {
      --new_basename_size;
    }

    // Now we will output the new basename after the root.
    pos += output_sized(buffer, buffer_size, pos, new_basename,
      new_basename_size);

    // And finally terminate the output and return the total size of the path.
    terminate_output(buffer, buffer_size, pos);
    return pos;
  }

  // If there is a last segment we can just forward this call, which is fairly
  // easy.
  return change_segment(&segment, new_basename, buffer, buffer_size);
}

void cwk::get_dirname(const char *path, size_t *length)
{
  struct cwk_segment segment;

  // We get the last segment of the path. The last segment will contain the
  // basename if there is any. If there are no segments we will set the length
  // to 0.
  if (!get_last_segment(path, &segment)) {
    *length = 0;
    return;
  }

  // We can now return the length from the beginning of the string up to the
  // beginning of the last segment.
  *length = (size_t)(segment.begin - path);
}

bool cwk::get_extension(const char *path, const char **extension,
  size_t *length)
{
  struct cwk_segment segment;
  const char *c;

  // We get the last segment of the path. The last segment will contain the
  // extension if there is any.
  if (!get_last_segment(path, &segment)) {
    return false;
  }

  // Now we search for a dot within the segment. If there is a dot, we consider
  // the rest of the segment the extension. We do this from the end towards the
  // beginning, since we want to find the last dot.
  for (c = segment.end; c >= segment.begin; --c) {
    if (*c == '.') {
      // Okay, we found an extension. We can stop looking now.
      *extension = c;
      *length = (size_t)(segment.end - c);
      return true;
    }
  }

  // We couldn't find any extension.
  return false;
}

bool cwk::has_extension(const char *path)
{
  const char *extension;
  size_t length;

  // We just wrap the get_extension call which will then do the work for us.
  return get_extension(path, &extension, &length);
}

size_t cwk::change_extension(const char *path, const char *new_extension,
  char *buffer, size_t buffer_size)
{
  struct cwk_segment segment;
  const char *c, *old_extension;
  size_t pos, root_size, trail_size, new_extension_size;

  // First we try to get the last segment. We may only have a root without any
  // segments, in which case we will create one.
  if (!get_last_segment(path, &segment)) {

    // So there is no segment in this path. First we grab the root and output
    // that. We are not going to modify the root in any way. If there is no
    // root, this will end up with a root size 0, and nothing will be written.
    get_root(path, &root_size);
    pos = output_sized(buffer, buffer_size, 0, path, root_size);

    // Add a dot if the submitted value doesn't have any.
    if (*new_extension != '.') {
      pos += output_dot(buffer, buffer_size, pos);
    }

    // And finally terminate the output and return the total size of the path.
    pos += output(buffer, buffer_size, pos, new_extension);
    terminate_output(buffer, buffer_size, pos);
    return pos;
  }

  // Now we seek the old extension in the last segment, which we will replace
  // with the new one. If there is no old extension, it will point to the end of
  // the segment.
  old_extension = segment.end;
  for (c = segment.begin; c < segment.end; ++c) {
    if (*c == '.') {
      old_extension = c;
    }
  }

  pos = output_sized(buffer, buffer_size, 0, segment.path,
    (size_t)(old_extension - segment.path));

  // If the new extension starts with a dot, we will skip that dot. We always
  // output exactly one dot before the extension. If the extension contains
  // multiple dots, we will output those as part of the extension.
  if (*new_extension == '.') {
    ++new_extension;
  }

  // We calculate the size of the new extension, including the dot, in order to
  // output the trail - which is any part of the path coming after the
  // extension. We must output this first, since the buffer may overlap with the
  // submitted path - and it would be overridden by longer extensions.
  new_extension_size = strlen(new_extension) + 1;
  trail_size = output(buffer, buffer_size, pos + new_extension_size,
    segment.end);

  // Finally we output the dot and the new extension. The new extension itself
  // doesn't contain the dot anymore, so we must output that first.
  pos += output_dot(buffer, buffer_size, pos);
  pos += output(buffer, buffer_size, pos, new_extension);

  // Now we terminate the output with a null-terminating character, but before
  // we do that we must add the size of the trail to the position which we
  // output before.
  pos += trail_size;
  terminate_output(buffer, buffer_size, pos);

  // And the position is our output size now.
  return pos;
}

size_t cwk::normalize(const char *path, char *buffer, size_t buffer_size)
{
  const char *paths[2];

  // Now we initialize the paths which we will normalize. Since this function
  // only supports submitting a single path, we will only add that one.
  paths[0] = path;
  paths[1] = NULL;

  return join_and_normalize_multiple(paths, buffer, buffer_size);
}

size_t cwk::get_intersection(const char *path_base, const char *path_other)
{
  bool absolute;
  size_t base_root_length, other_root_length;
  const char *end;
  const char *paths_base[2], *paths_other[2];
  struct cwk_segment_joined base, other;

  // We first compare the two roots. We just return zero if they are not equal.
  // This will also happen to return zero if the paths are mixed relative and
  // absolute.
  get_root(path_base, &base_root_length);
  get_root(path_other, &other_root_length);
  if (!is_string_equal(path_base, path_other, base_root_length)) {
    return 0;
  }

  // Configure our paths. We just have a single path in here for now.
  paths_base[0] = path_base;
  paths_base[1] = NULL;
  paths_other[0] = path_other;
  paths_other[1] = NULL;

  // So we get the first segment of both paths. If one of those paths don't have
  // any segment, we will return 0.
  if (!get_first_segment_joined(paths_base, &base) ||
      !get_first_segment_joined(paths_other, &other)) {
    return base_root_length;
  }

  // We now determine whether the path is absolute or not. This is required
  // because if will ignore removed segments, and this behaves differently if
  // the path is absolute. However, we only need to check the base path because
  // we are guaranteed that both paths are either relative or absolute.
  absolute = is_root_absolute(path_base, base_root_length);

  // We must keep track of the end of the previous segment. Initially, this is
  // set to the beginning of the path. This means that 0 is returned if the
  // first segment is not equal.
  end = path_base + base_root_length;

  // Now we loop over both segments until one of them reaches the end or their
  // contents are not equal.
  do {
    // We skip all segments which will be removed in each path, since we want to
    // know about the true path.
    if (!segment_joined_skip_invisible(&base, absolute) ||
        !segment_joined_skip_invisible(&other, absolute)) {
      break;
    }

    if (!is_string_equal(base.segment.begin, other.segment.begin,
          base.segment.size)) {
      // So the content of those two segments are not equal. We will return the
      // size up to the beginning.
      return (size_t)(end - path_base);
    }

    // Remember the end of the previous segment before we go to the next one.
    end = base.segment.end;
  } while (get_next_segment_joined(&base) &&
           get_next_segment_joined(&other));

  // Now we calculate the length up to the last point where our paths pointed to
  // the same place.
  return (size_t)(end - path_base);
}

bool cwk::get_first_segment(const char *path, struct cwk_segment *segment)
{
  size_t length;
  const char *segments;

  // We skip the root since that's not part of the first segment. The root is
  // treated as a separate entity.
  get_root(path, &length);
  segments = path + length;

  // Now, after we skipped the root we can continue and find the actual segment
  // content.
  return get_first_segment_without_root(path, segments, segment);
}

bool cwk::get_last_segment(const char *path, struct cwk_segment *segment)
{
  // We first grab the first segment. This might be our last segment as well,
  // but we don't know yet. There is no last segment if there is no first
  // segment, so we return false in that case.
  if (!get_first_segment(path, segment)) {
    return false;
  }

  // Now we find our last segment. The segment struct of the caller
  // will contain the last segment, since the function we call here will not
  // change the segment struct when it reaches the end.
  while (get_next_segment(segment)) {
    // We just loop until there is no other segment left.
  }

  return true;
}

bool cwk::get_next_segment(struct cwk_segment *segment)
{
  const char *c;

  // First we jump to the end of the previous segment. The first character must
  // be either a '\0' or a separator.
  c = segment->begin + segment->size;
  if (*c == '\0') {
    return false;
  }

  // Now we skip all separator until we reach something else. We are not yet
  // guaranteed to have a segment, since the string could just end afterwards.
  assert(is_separator(c));
  do {
    ++c;
  } while (is_separator(c));

  // If the string ends here, we can safely assume that there is no other
  // segment after this one.
  if (*c == '\0') {
    return false;
  }

  // Now we are safe to assume there is a segment. We store the beginning of
  // this segment in the segment struct of the caller.
  segment->begin = c;

  // And now determine the size of this segment, and store it in the struct of
  // the caller as well.
  c = find_next_stop(c);
  segment->end = c;
  segment->size = (size_t)(c - segment->begin);

  // Tell the caller that we found a segment.
  return true;
}

bool cwk::get_previous_segment(struct cwk_segment *segment)
{
  const char *c;

  // The current position might point to the first character of the path, which
  // means there are no previous segments available.
  c = segment->begin;
  if (c <= segment->segments) {
    return false;
  }

  // We move towards the beginning of the path until we either reached the
  // beginning or the character is no separator anymore.
  do {
    --c;
    if (c < segment->segments) {
      // So we reached the beginning here and there is no segment. So we return
      // false and don't change the segment structure submitted by the caller.
      return false;
    }
  } while (is_separator(c));

  // We are guaranteed now that there is another segment, since we moved before
  // the previous separator and did not reach the segment path beginning.
  segment->end = c + 1;
  segment->begin = find_previous_stop(segment->segments, c);
  segment->size = (size_t)(segment->end - segment->begin);

  return true;
}

enum cwk_segment_type cwk::get_segment_type(
  const struct cwk_segment *segment)
{
  // We just make a string comparison with the segment contents and return the
  // appropriate type.
  if (strncmp(segment->begin, ".", segment->size) == 0) {
    return CWK_CURRENT;
  } else if (strncmp(segment->begin, "..", segment->size) == 0) {
    return CWK_BACK;
  }

  return CWK_NORMAL;
}

bool cwk::is_separator(const char *str)
{
  const char *c;

  // We loop over all characters in the read symbols.
  c = separators[path_style];
  while (*c) {
    if (*c == *str) {
      return true;
    }

    ++c;
  }

  return false;
}

size_t cwk::change_segment(struct cwk_segment *segment, const char *value,
  char *buffer, size_t buffer_size)
{
  size_t pos, value_size, tail_size;

  // First we have to output the head, which is the whole string up to the
  // beginning of the segment. This part of the path will just stay the same.
  pos = output_sized(buffer, buffer_size, 0, segment->path,
    (size_t)(segment->begin - segment->path));

  // In order to trip the submitted value, we will skip any separator at the
  // beginning of it and behave as if it was never there.
  while (is_separator(value)) {
    ++value;
  }

  // Now we determine the length of the value. In order to do that we first
  // locate the '\0'.
  value_size = 0;
  while (value[value_size]) {
    ++value_size;
  }

  // Since we trim separators at the beginning and in the end of the value we
  // have to subtract from the size until there are either no more characters
  // left or the last character is no separator.
  while (value_size > 0 && is_separator(&value[value_size - 1])) {
    --value_size;
  }

  // We also have to determine the tail size, which is the part of the string
  // following the current segment. This part will not change.
  tail_size = strlen(segment->end);

  // Now we output the tail. We have to do that, because if the buffer and the
  // source are overlapping we would override the tail if the value is
  // increasing in length.
  output_sized(buffer, buffer_size, pos + value_size, segment->end,
    tail_size);

  // Finally we can output the value in the middle of the head and the tail,
  // where we have enough space to fit the whole trimmed value.
  pos += output_sized(buffer, buffer_size, pos, value, value_size);

  // Now we add the tail size to the current position and terminate the output -
  // basically, ensure that there is a '\0' at the end of the buffer.
  pos += tail_size;
  terminate_output(buffer, buffer_size, pos);

  // And now tell the caller how long the whole path would be.
  return pos;
}

enum cwk_path_style cwk::guess_style(const char *path)
{
  const char *c;
  size_t root_length;
  struct cwk_segment segment;

  // First we determine the root. Only windows roots can be longer than a single
  // slash, so if we can determine that it starts with something like "C:", we
  // know that this is a windows path.
  get_root_windows(path, &root_length);
  if (root_length > 1) {
    return CWK_STYLE_WINDOWS;
  }

  // Next we check for slashes. Windows uses backslashes, while unix uses
  // forward slashes. Windows actually supports both, but our best guess is to
  // assume windows with backslashes and unix with forward slashes.
  for (c = path; *c; ++c) {
    if (*c == *separators[CWK_STYLE_UNIX]) {
      return CWK_STYLE_UNIX;
    } else if (*c == *separators[CWK_STYLE_WINDOWS]) {
      return CWK_STYLE_WINDOWS;
    }
  }

  // This path does not have any slashes. We grab the last segment (which
  // actually must be the first one), and determine whether the segment starts
  // with a dot. A dot is a hidden folder or file in the UNIX world, in that
  // case we assume the path to have UNIX style.
  if (!get_last_segment(path, &segment)) {
    // We couldn't find any segments, so we default to a UNIX path style since
    // there is no way to make any assumptions.
    return CWK_STYLE_UNIX;
  }

  if (*segment.begin == '.') {
    return CWK_STYLE_UNIX;
  }

  // And finally we check whether the last segment contains a dot. If it
  // contains a dot, that might be an extension. Windows is more likely to have
  // file names with extensions, so our guess would be windows.
  for (c = segment.begin; *c; ++c) {
    if (*c == '.') {
      return CWK_STYLE_WINDOWS;
    }
  }

  // All our checks failed, so we will return a default value which is currently
  // UNIX.
  return CWK_STYLE_UNIX;
}

void cwk::set_style(enum cwk_path_style style)
{
  // We can just set the global path style variable and then the behaviour for
  // all functions will change accordingly.
  assert(style == CWK_STYLE_UNIX || style == CWK_STYLE_WINDOWS);
  path_style = style;
}

enum cwk_path_style cwk::get_style(void)
{
  // Simply return the path style which we store in a global variable.
  return path_style;
}
