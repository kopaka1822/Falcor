import unittest

# define class Face
class Face:

    # pos: position of the face
    # frontFace: bool if the face is the front face
    def __init__(self, pos, frontFace):
        self.pos = pos
        self.frontFace = frontFace


# gets non occluded area in [sphereStart, sphereEnd]
def get_visibility(faces, sphereStart, sphereEnd):

    sphereHeight = sphereEnd - sphereStart

    firstFrontFaceInSphere = sphereEnd
    firstBackFaceInSphere = sphereEnd
    firstFrontFaceBeforeSphere = 0
    firstBackFaceBeforeSphere = 0

    # iterate over all faces
    for face in faces:
        #skip invalid faces
        if face.pos < 0.0 or face.pos > sphereEnd:
            continue

        if face.frontFace:
            if face.pos > sphereStart:
                if face.pos < firstFrontFaceInSphere:
                    firstFrontFaceInSphere = face.pos
            else: # face.pos <= sphereStart
                if face.pos > firstFrontFaceBeforeSphere:
                    firstFrontFaceBeforeSphere = face.pos
        else: # back face
            if face.pos > sphereStart:
                if face.pos < firstBackFaceInSphere:
                    firstBackFaceInSphere = face.pos
            else: # face.pos <= sphereStart
                if face.pos > firstBackFaceBeforeSphere:
                    firstBackFaceBeforeSphere = face.pos

    area = sphereHeight - (sphereEnd - firstFrontFaceInSphere)
    if firstBackFaceInSphere < firstFrontFaceInSphere:
        area = area - (firstBackFaceInSphere - sphereStart)

    


class TestVisibility(unittest.TestCase):

    def test_empty(self):
        self.assertEqual(get_visibility([], 4.0, 2.0), 2.0)

    # simple case with one face (completely inside)    
    def test_In(self):
        faces = [Face(1.0, True), Face(3.0, False)]
        self.assertAlmostEqual(get_visibility(faces, 4.0, 4.0), 2.0)

    # simple case with one face (partially inside)    
    def test_InOut(self):
        faces = [Face(1.0, True), Face(3.0, False)]
        self.assertAlmostEqual(get_visibility(faces, 4.0, 2.0), 1.0)

    # face completely outside
    def test_Out(self):
        faces = [Face(1.0, True), Face(1.5, False)]
        self.assertAlmostEqual(get_visibility(faces, 4.0, 2.0), 2.0)

    # front face inside, back face outside
    def test_InOutOut(self):
        faces = [Face(3.5, True), Face(5, False)]
        self.assertAlmostEqual(get_visibility(faces, 4.0, 2.0), 1.5)

    # face surrounds the sampling radius
    def test_surround(self):
        faces = [Face(1.0, True), Face(5.0, False)]
        self.assertAlmostEqual(get_visibility(faces, 4.0, 2.0), 0.0)

    # two non overlapping faces inside 
    def test_2In(self):
        # ----FF-F--
        faces = [Face(2.0, True), Face(3.0, False), Face(3.5, True), Face(4.0, False)]
        self.assertAlmostEqual(get_visibility(faces, 5.0, 4.0), 2.5)

    def test_2InOverlap(self):
        # ----FFFF--
        faces = [Face(2.0, True), Face(3.5, False), Face(3.0, True), Face(4.0, False)]
        self.assertAlmostEqual(get_visibility(faces, 5.0, 4.0), 2.0)

    def test_2InOut(self):
        # 0012222100
        faces = [Face(1.0, True), Face(4.0, False), Face(1.5, True), Face(3.5, False)]
        self.assertAlmostEqual(get_visibility(faces, 5.0, 2.0), 1.0)

unittest.main()
